FROM alpine:latest AS builder
WORKDIR /netsrv
RUN ["apk", "update"]
RUN ["apk", "add", "make", "g++", "git"]
RUN ["git", "clone", "--recursive", "https://github.com/ilteoood/ps3netsrv--"]
WORKDIR /netsrv/ps3netsrv--
RUN ["git", "submodule", "update", "--init"]
RUN ["make", "CXX=g++"]

FROM alpine:latest
RUN ["apk", "add", "g++"]
ENV GAMES_FOLDER=/games
COPY --from=builder /netsrv/ps3netsrv--/ps3netsrv++ /ps3netsrv++
ENTRYPOINT ["sh", "-c", "mkdir -p $GAMES_FOLDER && /ps3netsrv++ $GAMES_FOLDER"]
