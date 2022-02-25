#Build image
FROM ubuntu:18.04 as build

# Install dependencies
RUN apt-get -y update
RUN apt-get -y install python3-pip unzip wget build-essential gcc g++ automake git-core autoconf make patch libmysql++-dev software-properties-common libtool libssl-dev grep binutils zlibc libc6 libbz2-dev cmake subversion libboost-all-dev
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | apt-key add -
RUN apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main'
RUN apt-get install cmake -y

# Download client @ https://drive.google.com/file/d/144bITQCPELoY-BoJetXy8dzbd21cPqoL/view?usp=sharing
RUN pip3 install gdown
RUN gdown --id 144bITQCPELoY-BoJetXy8dzbd21cPqoL --output tbc.zip
RUN unzip tbc.zip -d client

COPY . /src
WORKDIR /src/build

#Build and install mangos
RUN cmake .. -DCMAKE_INSTALL_PREFIX=/mangos -DBUILD_EXTRACTORS=ON -DPCH=1 -DDEBUG=0 -DBUILD_GAME_SERVER=1  -DBUILD_LOGIN_SERVER=1 -DBUILD_PLAYERBOT=ON -DBUILD_AHBOT=ON
RUN make -j4
RUN make install

#Extract resources
WORKDIR /client
RUN cp /src/contrib/extractor_scripts/* . 
RUN cp /mangos/bin/tools/* .
RUN chmod u+x ExtractResources.sh
RUN ./ExtractResources.sh a

#Runtime image
FROM ubuntu:18.04 as runtime

RUN apt-get -y update && apt-get -y upgrade
RUN apt-get -y install libmysqlclient20 openssl mysql-client git

COPY --from=build /mangos /mangos
WORKDIR /mangos/bin
RUN chmod +x mangosd
RUN chmod +x realmd
COPY --from=build /src/docker/entrypoint.sh /entrypoint.sh

#Copy config files to final image tmp directory
RUN mkdir /mangos/tmp/
WORKDIR /mangos/etc/
RUN for f in *.dist; do cp "$f" ../tmp/"${f%%.conf.dist}.conf"; done

#Copy sql files for initialization
COPY --from=build /src/sql /mangos/sql

#Copy extracted resources to runtime image
COPY --from=build /client/dbc /mangos/data
COPY --from=build /client/maps /mangos/data
COPY --from=build /client/vmaps /mangos/data
COPY --from=build /client/mmaps /mangos/data

#Get sql scripts
WORKDIR /mangos/db
RUN git clone https://github.com/cmangos/tbc-db

EXPOSE 8085
EXPOSE 3724
EXPOSE 3443

ENTRYPOINT [ "/bin/bash", "/entrypoint.sh" ]
