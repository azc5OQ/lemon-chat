patchedstunnel.tar.gz
contains files for patched stunnel server version 5.75
this stunnel appends X-Forwarded-for header to http requests
this was needed in case client.html is used within https secured website.
Websocket library that server is using (theldus websocket) doesnt support ssl natively (if it would, i wouldnt use it, since i wanted something lightweight) so stunnel is used in front of this. In case
basically this exists so ip address is not lost (websocket sees client as 127.0.0.1 if client connects through stunnel)
bear in mind, if stunnel is used, then clients can connect to both wss and ws ports.
I only tested stunnel in linux but it is said to work under windows too.
warning, client could theoretically trick server and send own X-Forwarded-for header, so this needs to be taken care of.
Also, if incoming address isnt 127.0.0.1, use clients address to find out what flag he has

