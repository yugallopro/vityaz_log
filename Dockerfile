FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -y && apt-get install -y g++ libssl-dev && apt-get clean
WORKDIR /app
COPY server.cpp .
RUN g++ server.cpp -o server -std=c++17 -lpthread -lssl -lcrypto
EXPOSE 8080
CMD ["./server"]
