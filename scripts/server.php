<?php
$ctx = new ZMQContext();
$socket = $ctx->getSocket(ZMQ::SOCKET_PULL);
$socket->bind("tcp://127.0.0.1:5555");

echo "Starting server\n";

while (true) {
	
	$message = $socket->recv();
	
	echo "\n--- start first part ---\n{$message}\n--- end first part ---\n";

	/* Second part coming? */
	if ($socket->getSockOpt(ZMQ::SOCKOPT_RCVMORE)) {
		
		$message = $socket->recv();
		echo "\n--- start second part ---\n{$message}\n--- end second part ---\n";
	}
}


