#!/bin/zsh -m

set -euo pipefail

TMPDIR="$(mktemp -d)"
export TMPDIR
trap '(( $(jobs | wc -l) > 0 )) && kill -9 $(jobs -p | awk "{ print \$3 }" ); rm -rf ${TMPDIR}' EXIT

# try 10 times to find an open port - sometimes there is a race condition of a
# port getting taken between check and server startup - but 10 times in a row
# is rather unlikely
for try in $(seq 10); do
    # find unused port
    while
        quiggeldy_port="$(shuf -n 1 -i 1024-65535 --random-source=/dev/urandom)"
        netstat -atun | grep -q "${quiggeldy_port}"
    do
      continue
    done

    echo "Found open port: ${quiggeldy_port}" >&2

    # assumes rcf-roundrobin-reinit-{client,server} to be in path
    # very very crude example demonstrating functionality
    rcf-roundrobin-reinit-server \
        --ip 127.0.0.1 \
        --port ${quiggeldy_port} \
        --timeout 30 \
        --loglevel ${RCF_TESTS_LOGLEVEL:-2} \
        --num-threads-input 6 \
        --num-threads-output 1 \
        --release-interval 5 \
        --user-period-ms 1000 \
        &
    pid_server="$!"
    sleep 1
    if kill -0 ${pid_server}; then
        break
    fi
done
echo "Server running (PID: ${pid_server})"

if ! kill -0 "${pid_server}"; then
    echo "Server did not start, aborting!" >&2
    exit 1
fi

result_file="$(mktemp)"
echo -n "" > ${result_file}

declare -a rcf_users
for idx_user in $(seq $((1 << 4))); do
    rcf_users+=(user_${idx_user})
done

for rcf_user in "${rcf_users[@]}"; do
    (set -e;
    session_num=0
    for i in $(seq $((1 << 1))); do
        ((session_num += 1))
        runtime="$(shuf -i 1-20 -n 1)"
        runtime_reinit="$(shuf -i 100-200 -n 1)"
        args=(
            --port ${quiggeldy_port}
            --session "session-${session_num}"
            --message "Testing by ${rcf_user}"
            --user ${rcf_user}
            --num-messages $((1 << 5))
            --runtime "${runtime}"
            --reinit-runtime "${runtime_reinit}"
            --synchronous  # TODO: There is still an unsolved issue with asynchronous submission and fast-forward.
        )
        # half of clients submits out-of-order
        if (( session_num % 2 == 0 )); then
            args+=(-o)
        fi
        rcf-roundrobin-reinit-client "${args[@]}" 2>&1 \
            | awk "{ printf(\"[${rcf_user}@${session_num}]: \"); print }"
    done
    echo "SUCCESS" >> ${result_file}
    ) &
done

sleep 1
# kill the last client in order to test a hard client failure
set +e
kill -9 $(pgrep -f -u $USER "rcf-roundrobin-reinit-client" | tail -n 1)
set -e

while [[ ${#jobstates} -gt 1 ]]; do
    echo "Still ${#jobstates} jobs running.."
    sleep 3
done

if kill -0 ${pid_server} &>/dev/null; then
    echo "Testing server signal-induced self-termination (PID: ${pid_server})" >&2
    kill ${pid_server}
    sleep 2
    if kill -0 ${pid_server} &>/dev/null; then
        echo "Server seems to be stuck, killing it.." >&2
        kill -9 ${pid_server}
        exit 1
    fi
fi
# get return code from server 
set +e
wait ${pid_server}
rc_server=$?
set -e

# We killed via sigterm so that is the exit code we expect
if (( ${rc_server} != 143 )); then
    echo "Server process encountered error!" >&2
    exit 1
fi

echo "Result: $(grep SUCCESS ${result_file} | wc -l)/${#rcf_users} clients exited successfully!" >&2
# check that all clients (except the one we killed) successfully ran through
if (( $(grep SUCCESS ${result_file} | wc -l) < (${#rcf_users}-1) )); then
    echo "ERROR: $((${#rcf_users}-1)) clients should have exited successfully!" >&2
    exit 1
fi

# Everthing went fine!
exit 0
