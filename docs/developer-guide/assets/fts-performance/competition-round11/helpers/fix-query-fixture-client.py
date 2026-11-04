from pathlib import Path

path = Path(r'D:\Projects\OceanBase\seekdb\unittest\storage\fts\test_query_materialization.py')
source = path.read_text(encoding='utf-8')
old = '    results = []\n    with pymysql.connect(**options) as connection:'
new = '''    from pymysql.constants import FIELD_TYPE
    conversions = pymysql.converters.conversions.copy()

    def decimal_value(value):
        # Preserve raw binary cells while decoding decimal protocol text.
        return Decimal(value.decode("ascii") if isinstance(value, bytes) else value)

    conversions[FIELD_TYPE.DECIMAL] = decimal_value
    conversions[FIELD_TYPE.NEWDECIMAL] = decimal_value
    options = dict(options, conv=conversions)
    results = []
    with pymysql.connect(**options) as connection:'''
assert source.count(old) == 1
path.write_text(source.replace(old, new), encoding='utf-8', newline='\n')
