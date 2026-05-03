FROM alpine:3.20 AS builder

RUN apk add --no-cache gcc musl-dev make ncurses-dev

WORKDIR /app
COPY src/ src/
COPY Makefile .

RUN make all

# --- Runtime image ---
FROM alpine:3.20

RUN apk add --no-cache libgcc ffmpeg ncurses-libs && \
    mkdir -p /videos/input /videos/segments /videos/processed /videos/final /videos/jobs

COPY --from=builder /app/bin/master  /usr/local/bin/master
COPY --from=builder /app/bin/worker  /usr/local/bin/worker
COPY --from=builder /app/bin/submit  /usr/local/bin/submit
