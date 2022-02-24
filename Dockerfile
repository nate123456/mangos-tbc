#Build image
FROM ubuntu:18.04 as build

RUN apt-get -y update
RUN apt-get -y install wget build-essential gcc g++ automake git-core autoconf make patch libmysql++-dev software-properties-common mysql-server libtool libssl-dev grep binutils zlibc libc6 libbz2-dev cmake subversion libboost-all-dev
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | apt-key add -
RUN apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main'
RUN apt-get install cmake -y

COPY . /src
COPY dep/ /dep
WORKDIR /src/build

#Install mangos
RUN cmake .. -DCMAKE_INSTALL_PREFIX=/mangos -DBUILD_EXTRACTORS=ON -DPCH=1 -DDEBUG=0 -DBUILD_GAME_SERVER=1  -DBUILD_LOGIN_SERVER=1 -DBUILD_PLAYERBOT=ON -DBUILD_AHBOT=ON
RUN make -j4
RUN make install

#Extract resources
FROM ubuntu:18.04 as extract
RUN pip install gdown
RUN sudo apt-get install unzip
RUN gdown --id 1ABYTDJM39e1nBIMO_mYFYdkcJK_oerya --output tbc.zip
RUN unzip tbc.zip -d client
WORKDIR /client
COPY --from=build /src/contrib/extractor_scripts/* . 
COPY --from=build /mangos/bin/tools/* . 
RUN chmod u+x ExtractResources.sh
RUN ./ExtractResources.sh a

FROM alpine:latest as db
RUN apk update && apk add git
RUN git clone https://github.com/cmangos/tbc-db

#Runtime image
FROM ubuntu:18.04 as runtime

RUN apt-get -y update && apt-get -y upgrade
RUN apt-get -y install libmysqlclient20 openssl mysql-client

COPY --from=build /mangos /mangos
WORKDIR /mangos/bin
RUN chmod +x mangosd
RUN chmod +x realmd
COPY --from=build /src/docker/mangosd/entrypoint.sh /entrypoint.sh

#Copy config files to final image tmp directory
RUN mkdir /mangos/tmp/
WORKDIR /mangos/etc/
RUN for f in *.dist; do cp "$f" ../tmp/"${f%%.conf.dist}.conf"; done

#Copy sql files for initialization
COPY --from=build /src/sql /mangos/sql

#Copy acquired extraction data to runtime image
COPY --from=extract /client/dbc /mangos/bin
COPY --from=extract /client/maps /mangos/bin
COPY --from=extract /client/vmaps /mangos/bin
COPY --from=extract /client/mmaps /mangos/bin

#Copy initialization sql scripts to runtime image
COPY --from=db /tbc-db /mangos/db

EXPOSE 8085
ENTRYPOINT [ "/bin/bash", "/entrypoint.sh" ]
