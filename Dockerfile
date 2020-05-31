FROM alpine:latest as builder
WORKDIR /netsrv
RUN ["apk", "update"]
RUN ["apk", "add", "make", "g++", "git"]
RUN ["git", "clone", "--recursive", "git://github.com/dirkvdb/ps3netsrv--.git"]
WORKDIR /netsrv/ps3netsrv--
RUN ["git", "submodule", "update", "--init"]
RUN ["make", "CXX=g++"]

FROM alpine:latest
RUN ["apk", "add", "g++"]
ENV GAMES_FOLDER /games
COPY --from=builder /netsrv/ps3netsrv--/ps3netsrv++ /ps3netsrv++
ENTRYPOINT sh -c "mkdir -p $GAMES_FOLDER && /ps3netsrv++ $GAMES_FOLDER"