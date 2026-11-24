# HTTP layer benchmark

nethttp and netrouter without sockets: routing with 300 literal routes,
the request parser on a head delivered in 64-byte pieces, on a chunked
body of 400 chunks delivered in 512-byte pieces and on 200 pipelined
requests, building an answer with 12 headers and 60 writes, and query /
header lookups. ns per call. `run.sh` builds it twice, with the units of
the working tree and with those of a commit, and runs both:

```sh
bench/http/run.sh                 # against main
bench/http/run.sh HEAD~3 1        # another commit, 1 second per case
```

(`P3=<compiler>` to use another p3.)
