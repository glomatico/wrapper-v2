FROM ubuntu:14.04@sha256:64483f3496c1373bfd55348e88694d1c4d0c9b660dee6bfef5e12f43b9933b30
RUN apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    bc file g++-multilib git libxml2-utils make openjdk-7-jdk python unzip zip \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
ENV LC_ALL=C TARGET_PRODUCT=sdk_phone_arm64 TARGET_BUILD_VARIANT=userdebug
