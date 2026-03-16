FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    g++ \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server.cpp .

RUN g++ server.cpp -o server -std=c++17 -lpthread -lpq

EXPOSE 8080
CMD ["./server"]
