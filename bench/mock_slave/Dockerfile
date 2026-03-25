FROM ubuntu:22.04
RUN apt-get update && apt-get install -y build-essential
COPY mock_slave /app/mock_slave
ENTRYPOINT ["/app/mock_slave"]
