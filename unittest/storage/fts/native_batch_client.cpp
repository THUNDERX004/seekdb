/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS


#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

class Connection {
 public:
  Connection() {
    WSADATA state;
    if (WSAStartup(MAKEWORD(2, 2), &state)) throw std::runtime_error("WSAStartup failed");
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) throw std::runtime_error("socket failed");
    int enabled = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&enabled), sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(2891);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
      throw std::runtime_error("dedicated test instance connect failed");
    }
    receive();
    // Protocol 4.1, secure auth response, native plugin, transactions, multiple results.
    const uint32_t capabilities = 1 | 4 | 512 | 8192 | 32768 | 131072 | 524288;
    std::string login(32, '\0');
    for (int i = 0; i < 4; ++i) login[i] = static_cast<char>(capabilities >> (8 * i));
    login[6] = 0xff;  // Maximum packet size 0x00ff0000.
    login[8] = 45;    // utf8mb4_general_ci.
    login.append("root", 4); login.push_back('\0');
    login.push_back('\0');
    login.append("mysql_native_password", 21); login.push_back('\0');
    send_packet(login, 1);
    std::string response = receive();
    if (!response.empty() && static_cast<unsigned char>(response[0]) == 0xfe) {
      send_packet("", 3);
      response = receive();
    }
    check_error(response);
    if (response.empty() || response[0] != 0) throw std::runtime_error("unsupported auth reply");
  }
  ~Connection() {
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
    WSACleanup();
  }
  void query(const std::string &sql) {
    send_packet(std::string(1, '\3') + sql, 0);
    std::string packet = receive();
    check_error(packet);
    if (packet.empty() || packet[0] == 0) return;
    size_t offset = 0;
    const uint64_t columns = lenenc(packet, offset);
    if (columns > 4096) throw std::runtime_error("invalid result column count");
    for (uint64_t i = 0; i < columns; ++i) check_error(receive());
    packet = receive();
    check_error(packet);
    if (!is_eof(packet)) throw std::runtime_error("expected field EOF");
    while (true) {
      packet = receive();
      check_error(packet);
      if (is_eof(packet)) break;
      offset = 0;
      for (uint64_t i = 0; i < columns; ++i) {
        if (i) std::fputc('\t', stdout);
        if (offset >= packet.size()) throw std::runtime_error("invalid row");
        if (static_cast<unsigned char>(packet[offset]) == 0xfb) {
          ++offset;
          std::fwrite("NULL", 1, 4, stdout);
        } else {
          uint64_t length = lenenc(packet, offset);
          if (length > packet.size() - offset) throw std::runtime_error("invalid row field");
          std::fwrite(packet.data() + offset, 1, static_cast<size_t>(length), stdout);
          offset += static_cast<size_t>(length);
        }
      }
      std::fputc('\n', stdout);
    }
  }
 private:
  SOCKET socket_ = INVALID_SOCKET;
  static bool is_eof(const std::string &value) {
    return !value.empty() && static_cast<unsigned char>(value[0]) == 0xfe && value.size() < 9;
  }
  static void check_error(const std::string &value) {
    if (!value.empty() && static_cast<unsigned char>(value[0]) == 0xff) {
      throw std::runtime_error(value.size() >= 9 ? value.substr(9) : "server error");
    }
  }
  static uint64_t lenenc(const std::string &value, size_t &offset) {
    if (offset >= value.size()) throw std::runtime_error("invalid length");
    unsigned char first = value[offset++];
    if (first < 0xfb) return first;
    int bytes = first == 0xfc ? 2 : first == 0xfd ? 3 : first == 0xfe ? 8 : 0;
    if (!bytes || offset + bytes > value.size()) throw std::runtime_error("invalid encoded length");
    uint64_t number = 0;
    for (int i = 0; i < bytes; ++i) number |= uint64_t(static_cast<unsigned char>(value[offset++])) << (8 * i);
    return number;
  }
  void read_exact(char *buffer, size_t count) {
    while (count) {
      int read = recv(socket_, buffer, static_cast<int>(count), 0);
      if (read <= 0) throw std::runtime_error("server disconnected");
      buffer += read; count -= read;
    }
  }
  std::string receive() {
    unsigned char header[4];
    read_exact(reinterpret_cast<char *>(header), 4);
    size_t length = header[0] | (size_t(header[1]) << 8) | (size_t(header[2]) << 16);
    std::string value(length, '\0');
    if (length) read_exact(value.data(), length);
    return value;
  }
  void send_packet(const std::string &value, unsigned char sequence) {
    if (value.size() >= 0xffffff) throw std::runtime_error("query packet too large");
    std::string packet(4, '\0');
    for (int i = 0; i < 3; ++i) packet[i] = static_cast<char>(value.size() >> (8 * i));
    packet[3] = sequence;
    packet += value;
    size_t offset = 0;
    while (offset < packet.size()) {
      int sent = send(socket_, packet.data() + offset, static_cast<int>(packet.size() - offset), 0);
      if (sent <= 0) throw std::runtime_error("send failed");
      offset += sent;
    }
  }
};

std::string utf8(const wchar_t *value) {
  int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  std::string result(length, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
  result.pop_back();
  return result;
}

int wmain(int count, wchar_t **arguments) {
  try {
    if (const char *password = std::getenv("SEEKDB_TEST_PASSWORD"); password && *password) {
      throw std::runtime_error("local native client requires a passwordless dedicated instance");
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::string sql;
    bool explicit_sql = false;
    for (int i = 1; i < count; ++i) {
      if (std::wcscmp(arguments[i], L"-e") == 0 && i + 1 < count) {
        sql = utf8(arguments[++i]); explicit_sql = true;
      }
    }
    if (!explicit_sql) {
      char buffer[65536];
      size_t length;
      while ((length = std::fread(buffer, 1, sizeof(buffer), stdin))) sql.append(buffer, length);
    }
    Connection connection;
    size_t start = 0;
    char quote = 0;
    bool escaped = false, line_comment = false, block_comment = false;
    for (size_t i = 0; i < sql.size(); ++i) {
      char ch = sql[i];
      if (line_comment) line_comment = ch != '\n';
      else if (block_comment) {
        if (ch == '*' && i + 1 < sql.size() && sql[i + 1] == '/') { block_comment = false; ++i; }
      } else if (quote) {
        if (escaped) escaped = false;
        else if (ch == '\\') escaped = true;
        else if (ch == quote) {
          if (i + 1 < sql.size() && sql[i + 1] == quote) ++i;
          else quote = 0;
        }
      } else if (ch == '\'' || ch == '"' || ch == char(96)) quote = ch;
      else if (ch == '#' || (ch == '-' && i + 2 < sql.size() && sql[i + 1] == '-' &&
                             std::isspace(static_cast<unsigned char>(sql[i + 2])))) line_comment = true;
      else if (ch == '/' && i + 1 < sql.size() && sql[i + 1] == '*') { block_comment = true; ++i; }
      else if (ch == ';') {
        if (sql.find_first_not_of(" \t\r\n", start) <= i) connection.query(sql.substr(start, i - start + 1));
        start = i + 1;
      }
    }
    if (sql.find_first_not_of(" \t\r\n", start) != std::string::npos) connection.query(sql.substr(start));
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "native-client error: %s\n", error.what());
    return 1;
  }
}
