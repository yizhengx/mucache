#!/bin/bash

cd $(dirname $0)

export SLOWPOKE_DELAY_MICROS_CART=1000
export SLOWPOKE_DELAY_MICROS_CHECKOUT=1000
export SLOWPOKE_DELAY_MICROS_CURRENCY=1000
export SLOWPOKE_DELAY_MICROS_EMAIL=1000
export SLOWPOKE_DELAY_MICROS_FRONTEND=1000
export SLOWPOKE_DELAY_MICROS_PAYMENT=1000
export SLOWPOKE_DELAY_MICROS_PRODUCT_CATALOG=1000
export SLOWPOKE_DELAY_MICROS_RECOMMENDATIONS=1000
export SLOWPOKE_DELAY_MICROS_SHIPPING=1000

request=home
thread=${1:-16}
conn=${2:-16}
rate=${3:-1000}
duration=60

# bash run.sh $request $thread $conn $duration
bash run-wrk2.sh $request $thread $conn $duration $rate