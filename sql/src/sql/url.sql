
CREATE TYPE url EXTERNAL NAME url;

CREATE table url_test ( theUrl url, name string );
INSERT into url_test values ( url 'http://monetdb.cwi.nl/', 'MonetDB');
select * from url_test;

CREATE function getAnchor( theUrl url ) RETURNS STRING 
	EXTERNAL NAME url."getAnchor";
CREATE function getBasename(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getBasename";
CREATE function getContent(theUrl url)   RETURNS STRING       
	EXTERNAL NAME url."getContent";
CREATE function getContext(theUrl url)   RETURNS STRING       
	EXTERNAL NAME url."getContext";
CREATE function getDomain(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getDomain";
CREATE function getExtension(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getExtension";
CREATE function getFile(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getFile";
CREATE function getHost(theUrl url)   RETURNS STRING       
	EXTERNAL NAME url."getHost";
CREATE function getPort(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getPort";
CREATE function getProtocol(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getProtocol";
CREATE function getQuery(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getQuery";
CREATE function getUser(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getUser";
CREATE function getRobotURL(theUrl url) RETURNS STRING       
	EXTERNAL NAME url."getRobotURL";
CREATE function isaURL(theUrl url) RETURNS BOOL
	EXTERNAL NAME url."isaURL";
CREATE function newurl(protocol STRING, hostname STRING,port INT, file STRING) 
	RETURNS url       
	EXTERNAL NAME url."newUrl";
CREATE function newurl(protocol STRING, hostname STRING, file STRING) 
	RETURNS url 
	EXTERNAL NAME url."newurl";
