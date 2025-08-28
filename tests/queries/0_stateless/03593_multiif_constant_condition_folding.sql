SELECT multiIf(atype = 1, IPv4NumToString(reinterpretAsUInt32(reverse(s))), atype = 28, IPv6NumToString(toFixedString(s, 16)), s)
FROM
(
    SELECT
        99 AS atype,
        'abcdefghijklmnopq' AS s
);

WITH test AS
    (
        SELECT 0 AS number
    )
SELECT if(number = 0, 0, intDiv(42, number))
FROM test;

SELECT if(1 = 1, 1, toInt8('x'));

SELECT multiIf(1 = 1, 1, 1 = 2, 2, toInt8('x'));

SELECT CASE WHEN 1=1 THEN 1 ELSE toInt8('x') END;

with 'kek_kkk_kkk_kkk_kkk' as c_s SELECT multiIf(atype <= 1, IPv4NumToString(reinterpretAsUInt32(reverse(s))), atype < 28, IPv6NumToString(toFixedString(c_s, 16)), s) FROM (SELECT 99 as atype, 'abcdefghijklmnopq' as s);
