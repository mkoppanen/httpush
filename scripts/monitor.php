<?php

$ctx = new ZMQContext();
$socket = $ctx->getSocket(ZMQ::SOCKET_REQ);
$socket->setSockOpt(ZMQ::SOCKOPT_IDENTITY, "test");

$socket->connect("tcp://localhost:5567");
$socket->send("stats", 0);

$xml = $socket->recv();
$sxe = simplexml_load_string($xml);

if (!$sxe)
	die("Failed to parse the response\n");

echo "Received responses from {$sxe->statistics->responses} out of {$sxe->statistics->threads} threads\n";
echo "Total requests: {$sxe->statistics->requests}\n";

foreach ($sxe->statistics->children() as $k => $v) {
	if ($k == "status") {
		echo "  HTTP code {$v['code']}: $v \n";
	}
}