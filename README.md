httpush
=======

What is httpush?
----------------

httpush is a small http web-server that pushes the messages to ZeroMQ socket.
The web-server part is implemented using libevent evhttpd and configurable
amount of worker threads. Each one of the worker threads run a separate event
loop and nothing is shared between the threads.

Command-line options
--------------------

<table>
	<tr>
		<td> option </td><td> type </td><td> default </td><td> description </td>
	</tr>
    <tr>
		<td> -b </td><td> string </td><td> 0.0.0.0 </td><td> Hostname or ip to for the HTTP daemon </td>
	</tr>
    <tr>
		<td> -d </td><td> flag </td><td> no </td><td> Daemonize the program </td>
	</tr>    
    <tr>     
		<td> -g </td><td> string </td><td> nobody </td><td> Group to run as </td>
	</tr>
    <tr>     
		<td> -l </td><td> integer </td><td> 2000 </td><td> ZeroMQ linger value (ZMQ_LINGER) </td>
	</tr>                         
    <tr>                          
		<td> -m </td><td> string </td><td> tcp://127.0.0.1:5555 </td><td> Bind dsn for ZeroMQ monitoring socket </td>
	</tr>                         
    <tr>                          
		<td> -o </td><td> flag </td><td> no </td><td> Optimize for bandwidth usage (exclude headers from messages) </td>
	</tr>
    <tr>     
		<td> -p </td><td> integer </td><td> 8080 </td><td> HTTPD listen port </td>
	</tr>                         
    <tr>                          
		<td> -s </td><td> string </td><td> 0 </td><td> Disk offload max size (G/M/k/B) (ZMQ_SWAP) </td>
	</tr>                         
    <tr>                          
		<td> -t </td><td> integer </td><td> 5 </td><td> Number of HTTPD threads </td>
	</tr>
    <tr>     
		<td> -u </td><td> string </td><td> nobody </td><td> User to run as </td>
	</tr>                         
    <tr>                          
		<td> -w </td><td> integer </td><td> 0 </td><td> The ZeroMQ high watermark limit (ZMQ_HWM) </td>
	</tr>                         
    <tr>                          
		<td> -z </td><td> string </td><td> tcp://127.0.0.1:5567 </td><td> Comma-separated list of zeromq URIs to connect to </td>
	</tr>
</table>
			

### -z connect uri format ###

The -z option allows a comma separated list of ZeroMQ uris to connect to.
Each one of the uris can contain additional hwm, swap and linger query
parameters. Example of a valid -z parameter:

"tcp://127.0.0.1:2233?hwm=5&swap=10M&linger=100,tcp://127.0.0.1:5555"

A socket that doesn't explicitly specify hwm, swap or linger will use the 
values defined by -s, -w and -l parameters.

Monitoring
----------

A monitoring socket can be used to query statistics about the server usage.
Example response from monitoring socket might contain:
<?xml version="1.0" encoding="UTF-8" ?>
 <httpush>
   <statistics>
     <threads>10</threads>
     <responses>10</responses>
     <requests>7</requests>
     <status code="200">7</status>
     <status code="404">0</status>
     <status code="412">0</status>
     <status code="503">0</status>
   </statistics>
</httpush>

