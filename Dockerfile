FROM alpine:3.20 AS builder

RUN apk add --no-cache gcc musl-dev make

WORKDIR /app
COPY src/ src/
COPY Makefile .

RUN make all

# --- Runtime image ---
FROM alpine:3.20

RUN apk add --no-cache libgcc

COPY --from=builder /app/bin/master /usr/local/bin/master
COPY --from=builder /app/bin/worker /usr/local/bin/worker
