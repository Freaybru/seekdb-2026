FROM gcc:13.3.0-bookworm@sha256:50e85821c43ea2fd683bf5ae41f5d6981c41bab0641e7640962759291ceb7f78 AS build
WORKDIR /build
COPY extra/sql.hpp extra/log.hpp extra/db.hpp extra/sql.cpp extra/log.cpp extra/db.cpp extra/main.cpp ./
RUN g++ -std=c++20 -O3 -flto -static -Wall -Wextra -Wpedantic \
    sql.cpp log.cpp db.cpp main.cpp -o /minivectordb \
    && strip /minivectordb && mkdir /data

FROM scratch
COPY --from=build /minivectordb /minivectordb
COPY --from=build /data /data
VOLUME ["/data"]
ENTRYPOINT ["/minivectordb"]
CMD ["--data-dir", "/data"]
