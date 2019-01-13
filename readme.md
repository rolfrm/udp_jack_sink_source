Test of streaming data to Jack from a network source.



#### Things to test
- Latency using TCP, UDP, DTLS.
- Percived audio quality, subject to packet drops
- LAN/WAN quality and latency
- Behavior subject to network congestion


#### Usage

`./run [--sink|--source] addr port`

For --sink use an address that is externally accessible like 0.0.0.0.

Setting up a sink use:

```./run --sink 0.0.0.0 8888```

Setting up a source:

```./run --source [ sink ip] 8888````