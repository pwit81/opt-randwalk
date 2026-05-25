FROM debian:trixie

WORKDIR /root

RUN apt-get -q update && apt-get upgrade -y
RUN apt-get install -y --no-install-recommends \
      git make gcc pkg-config valgrind ca-certificates libc6-dev python3
