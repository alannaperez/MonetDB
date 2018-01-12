
DROP SCHEMA IF EXISTS foo;
CREATE SCHEMA foo;
SET SCHEMA foo;

COMMENT ON SCHEMA foo IS 'foo foo';

CREATE TABLE tab(i INT, j DECIMAL(4,2));
COMMENT ON TABLE tab IS 'table';
COMMENT ON COLUMN tab.j IS 'jj';
COMMENT ON COLUMN foo.tab.i IS 'ii';

CREATE VIEW vivi AS SELECT * FROM tab;
COMMENT ON VIEW vivi IS 'phew';

CREATE INDEX idx ON tab(j,i);
COMMENT ON INDEX idx IS 'index on j';

CREATE SEQUENCE counter AS INT;
COMMENT ON SEQUENCE counter IS 'counting';


CREATE FUNCTION f() RETURNS INT BEGIN RETURN 42; END;
COMMENT ON FUNCTION f() IS '0 parms';

CREATE FUNCTION f(i INT) RETURNS INT BEGIN RETURN 43; END;
COMMENT ON FUNCTION f(INT) IS '1 parm';

CREATE FUNCTION f(i INT, j INT) RETURNS INT BEGIN RETURN 44; END;
COMMENT ON FUNCTION f(INTEGER, INTEGER) IS '2 parms';

CREATE FUNCTION f(i INT, j INT, k INT) RETURNS INT BEGIN RETURN 45; END;
CREATE FUNCTION f(i INT, j INT, k INT, l INT) RETURNS INT BEGIN RETURN 45; END;

CREATE PROCEDURE g() BEGIN DELETE FROM tab WHERE FALSE; END;
COMMENT ON PROCEDURE g() IS 'proc';
