#!/bin/bash

cd $(dirname $0)

# delete all services
echo "Deleting all services"
kubectl delete -f yamls/ --ignore-not-found=true

# deploy all services
echo "Deploying all services"
for file in $(ls -d yamls/*.yaml)
do
    envsubst < $file | kubectl apply -f - 
done

# wait until all pods are ready by checking the log to see if the "server started" message is printed
echo "Waiting for all pods to be ready"
while true
do
    res=$(kubectl get pods | cut -f 1 -d " " | grep -vE "ubuntu|NAME|redis")
    check=0
    IFS=$'\n' read -rd '' -a array <<< "$res"
    for value in "${array[@]:1:${#array[@]}-1}"
    do
        kubectl logs $value | grep "Server started" > /dev/null
        if [ $? -ne 0 ]
        then
            check=1
            # echo "waiting for $value"
            sleep 1
            break
        fi
    done
    if [ $check -eq 0 ]
    then
        echo "All pods are ready"
        break
    fi
done

# run the load generator

# grep the frontend service cluster-ip
frontend_ip=$(kubectl get svc | grep frontend | awk '{print $3}')

# run the load generator
echo "Running the load generator to $frontend_ip"
~/wrk/wrk -t8 -c128 -d60s -L -s ~/wrk/scripts/online-boutique/home.lua http://${frontend_ip}:80 &

sleep 30
kubectl top pods
sleep 40