START TRANSACTION;

CREATE TABLE rval(i integer,j integer);
INSERT INTO rval VALUES (1,4), (2,3), (3,2), (4,1);

CREATE FUNCTION pyapi02(i integer,j integer,z integer) returns integer
language P
{
    x = i * sum(j) * z;
    return(x);
};

SELECT pyapi02(i,j,2) as r02 FROM rval;
DROP FUNCTION pyapi02;
DROP TABLE rval;

ROLLBACK;

