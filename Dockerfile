FROM gcc:13

RUN apt-get update && apt-get install -y libpq-dev

WORKDIR /app
COPY server.cpp .

RUN g++ server.cpp -o server -std=c++17 -lpthread -lpq

EXPOSE 8080
CMD ["./server"]
