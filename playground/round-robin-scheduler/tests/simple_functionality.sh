#!/bin/zsh -m

# assumes rcf-roundrobin-{client,server} to be in path
# very very crude example demonstrating functionality
rcf-roundrobin-server -i 127.0.0.1 -p 8989 -n 4 -m 2 &
pid_server=$!
echo "Server running (PID: ${pid_server}})"

for u in mueller obreitwi mauch vkarasen dstoe mpedro; do
    rcf-roundrobin-client -p 8989 -m "Testing done by ${u}" -u ${u} -n 100 -q -r 25 &
done

# some later batches
for u in obreitwi mauch mpedro; do
    rcf-roundrobin-client -p 8989 -m "Testing batch #2 done by ${u}" -u ${u} -n 100 -q -r 25 &
done

# mueller tries again
for u in mueller; do
    rcf-roundrobin-client -p 8989 -m "Testing done #3 by ${u}" -u ${u} -n 100 -q -r 25 &
done

while [[ ${#jobstates} -gt 1 ]]; do
    echo "Still ${#jobstates} jobs running.."
    sleep 3
done

echo "Killing server process (PID: ${pid_server})"
kill ${pid_server}
wait

