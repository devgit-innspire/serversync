# Linux build of server for Render. Ubuntu's libmysqlclient-dev is real MySQL 8 (Debian ships MariaDB, whose headers differ).
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ libmysqlclient-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY include include
COPY src src
RUN g++ -std=c++20 -O2 -Wall -Wextra -o server src/*.cpp -Iinclude $(mysql_config --cflags) $(mysql_config --libs) -pthread

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libmysqlclient21 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /app/server .
COPY public public
COPY templates templates
# PORT in common.h is 8080; render.yaml sets PORT=8080 so Render routes traffic here
EXPOSE 8080
CMD ["./server"]
