#!/bin/zsh -m

pidfile="$(mktemp)"

trap '(( $(jobs | wc -l) > 0 )) && kill $(jobs -p | awk "{ print $3 }" )' EXIT

# assumes rcf-roundrobin-{client,server} to be in path
# very very crude example demonstrating functionality
( ( rcf-roundrobin-server -i 127.0.0.1 \
                          -p 8989 \
                          -t 10 \
                          -n 4 \
                          -m 2 \
                          2>&1 &; echo $! > "${pidfile}" ) | tee server.log ) &
pid_server=$(cat "${pidfile}")
rm "${pidfile}"
echo "Server running (PID: ${pid_server})"

if ! kill -0 "${pid_server}"; then
    echo "Server did not start, aborting!" >&2
    exit 1
fi


for u in mueller obreitwi mauch vkarasen dstoe mpedro; do
    rcf-roundrobin-client -p 8989 -m "Testing by ${u}" -u ${u} -n 100 -q -r 35 &
done

# some later batches
for u in obreitwi mauch mpedro; do
    rcf-roundrobin-client -p 8989 -m "Testing batch #2 by ${u}" -u ${u} -n 100 -q -r 15 &
done

# mueller tries again
for u in mueller; do
    rcf-roundrobin-client -p 8989 -m "Testing batch #3 by ${u}" -u ${u} -n 100 -q -r 45 &
done

while [[ ${#jobstates} -gt 1 ]]; do
    echo "Still ${#jobstates} jobs running.."
    sleep 3
done

echo "Killing server process (PID: ${pid_server})"
kill ${pid_server}
wait

