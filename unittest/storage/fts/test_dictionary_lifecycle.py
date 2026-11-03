#!/usr/bin/env python3
"""Shared FTS dictionary startup/shutdown regression using the public C driver.

Set SEEKDB_TEST_DRIVER and SEEKDB_TEST_BINARY to the candidate DLL/server.
Runs two normal open/close cycles on a dedicated temporary instance, verifies
TOKENIZE, persistent fulltext indexes and MATCH through both cycles. Intended
for Windows native builds; no installed service or user database is touched.
"""
import ctypes
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
import unittest

class Driver:
    def __init__(self, library, binary):
        self.dll_directory = os.add_dll_directory(str(library.parent))
        self.lib = ctypes.CDLL(str(library))
        signatures = {
            "seekdb_set_binary_path": ([ctypes.c_char_p], ctypes.c_int),
            "seekdb_open": ([ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int),
            "seekdb_last_open_error": ([], ctypes.c_char_p),
            "seekdb_close": ([ctypes.c_void_p], ctypes.c_int),
            "seekdb_connect": ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_bool, ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int),
            "seekdb_disconnect": ([ctypes.c_void_p], ctypes.c_int),
            "seekdb_query": ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int64, ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int),
            "seekdb_last_error": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_char_p)], ctypes.c_int),
            "seekdb_result_free": ([ctypes.c_void_p], ctypes.c_int),
            "seekdb_result_next": ([ctypes.c_void_p], ctypes.c_int),
            "seekdb_result_get_int64": ([ctypes.c_void_p, ctypes.c_int64, ctypes.POINTER(ctypes.c_int64)], ctypes.c_int),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes = arguments
            function.restype = result
        if self.lib.seekdb_set_binary_path(str(binary).encode("utf-8")) != 0:
            raise RuntimeError("Cannot select the server executable")

    def open(self, directory):
        pairs = [b"cpu_count", b"2", b"memory_budget", b"2G", b"log_disk_size", b"2G"]
        pairs.append(None)
        parameters = (ctypes.c_char_p * len(pairs))(*pairs)
        handle = ctypes.c_void_p()
        code = self.lib.seekdb_open(str(directory).encode("utf-8"), parameters, ctypes.byref(handle))
        if code != 0:
            error = self.lib.seekdb_last_open_error()
            message = error.decode("utf-8", errors="replace") if error else "unknown startup failure"
            if handle.value:
                self.lib.seekdb_close(handle)
            raise RuntimeError(f"seekdb_open failed ({code}): {message}")
        return handle

    def execute(self, handle, sql, scalar=False):
        connection = ctypes.c_void_p()
        if self.lib.seekdb_connect(handle, b"test", True, ctypes.byref(connection)) != 0:
            raise RuntimeError("Cannot connect to the started instance")
        result = ctypes.c_void_p()
        try:
            encoded = sql.encode("utf-8")
            if self.lib.seekdb_query(connection, encoded, len(encoded), ctypes.byref(result)) != 0:
                number, message = ctypes.c_int(), ctypes.c_char_p()
                self.lib.seekdb_last_error(connection, ctypes.byref(number), ctypes.byref(message))
                detail = message.value.decode("utf-8", errors="replace") if message.value else "unknown SQL error"
                raise RuntimeError(f"SQL error {number.value}: {detail}")
            if scalar:
                if not result.value or self.lib.seekdb_result_next(result) != 0:
                    raise RuntimeError("Expected one result row")
                value = ctypes.c_int64()
                if self.lib.seekdb_result_get_int64(result, 0, ctypes.byref(value)) != 0:
                    raise RuntimeError("Cannot read the integer result")
                return value.value
        finally:
            if result.value:
                self.lib.seekdb_result_free(result)
            self.lib.seekdb_disconnect(connection)



@unittest.skipUnless(sys.platform == "win32", "uses the Windows C driver")
class DictionaryLifecycleTest(unittest.TestCase):
    def test_close_reopen(self):
        library = os.environ.get("SEEKDB_TEST_DRIVER")
        binary = os.environ.get("SEEKDB_TEST_BINARY")
        if not library or not binary:
            self.skipTest("Set SEEKDB_TEST_DRIVER and SEEKDB_TEST_BINARY")
        driver = Driver(Path(library).resolve(strict=True), Path(binary).resolve(strict=True))
        directory = Path(tempfile.mkdtemp(prefix="fts-dict-lifecycle-")).resolve()
        previous = os.environ.get("TELEMETRY_ENABLED")
        os.environ["TELEMETRY_ENABLED"] = "false"
        passed = False
        try:
            for cycle in range(2):
                handle = driver.open(directory)
                try:
                    # Evaluating TOKENIZE before and after close exercises dictionary
                    # publication, pin lifetime and rebuilding readers on reopen.
                    for parser in ["ik", "beng"]:
                        value = driver.execute(handle,
                            "SELECT TOKENIZE('Database 全文索引 分词器','" + parser + "') IS NOT NULL",
                            scalar=True)
                        self.assertEqual(1, value)
                    if cycle == 0:
                        driver.execute(handle, "CREATE TABLE fts_lifecycle(id INT PRIMARY KEY, body TEXT)")
                        driver.execute(handle, "INSERT INTO fts_lifecycle VALUES (1,'数据库 全文索引'),(2,'数据库 分词器'),(3,'hello')")
                        driver.execute(handle, "ALTER TABLE fts_lifecycle ADD FULLTEXT INDEX ft(body) WITH PARSER ik")
                    self.assertEqual(2, driver.execute(handle,
                        "SELECT COUNT(*) FROM fts_lifecycle WHERE MATCH(body) AGAINST('数据库')", scalar=True))
                    self.assertEqual(2, driver.execute(handle,
                        "SELECT COUNT(*) FROM fts_lifecycle WHERE MATCH(body) AGAINST('数据库')>0", scalar=True))
                finally:
                    self.assertEqual(0, driver.lib.seekdb_close(handle))
            passed = True
        finally:
            driver.dll_directory.close()
            if previous is None:
                os.environ.pop("TELEMETRY_ENABLED", None)
            else:
                os.environ["TELEMETRY_ENABLED"] = previous
            if not passed:
                print("Retained failed instance:", directory, file=sys.stderr)
            else:
                if directory.parent != Path(tempfile.gettempdir()).resolve() or not directory.name.startswith("fts-dict-lifecycle-"):
                    raise RuntimeError("Unexpected temporary instance directory")
                deadline = time.monotonic() + 10
                while True:
                    try:
                        shutil.rmtree(directory)
                        break
                    except PermissionError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.1)


if __name__ == "__main__":
    unittest.main()
