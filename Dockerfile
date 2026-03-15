FROM gcc:13

WORKDIR /app
COPY server.cpp .

RUN g++ server.cpp -o server -std=c++17 -lpthread

EXPOSE 8080
CMD ["./server"]
