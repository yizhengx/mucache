#!/bin/bash

cd $(dirname $0)

# Loop through each folder in the current directory
for dir in ./*; do
    cd "$dir" || continue

    if [[ ! -d "yamls" ]]; then
        echo "No yamls directory in $dir. Skipping..."
        cd ..
        continue
    fi

    # Create archive folder if it doesn't exist
    mkdir -p archive-opt

    # Move all folders except *.yaml into archive
    for item in *; do
        if [ -d "$item" ] && [[ "$item" != "archive-opt" ]] && [[ "$item" != "yamls" ]]; then
            mv "$item" archive-opt/
        fi
    done

    # Loop through each serviceX.yaml file
    cd yamls || continue
    for yaml in *; do
        [[ -f "$yaml" ]] || continue

        # Extract the service name, e.g., "service1" from "service1.yaml"
        servicename="${yaml%%.*}"  # remove .yaml
        servicename="${yaml%%.yaml}"

        echo "Processing $yaml for service $servicename"

        # Insert two lines after SLOWPOKE_IS_TARGET_SERVICE line
        awk -v svc="$servicename" '
        {
            print $0
            if ($0 ~ /SLOWPOKE_IS_TARGET_SERVICE/) {
                getline; print $0
                print "                    - name: SLOWPOKE_NEIGHBORS"
                print "                      value: \"${SLOWPOKE_NEIGHBORS_" toupper(svc) "}\""
                next
            }
        }
        ' "$yaml" | sed 's|yizhengx/mucache:synthetic-pokerpp-0maxconn-grpc|yizhengx/mucache-pokerpp-opt|g' | sed 's|yizhengx/mucache:synthetic-pokerpp-0maxconn|yizhengx/mucache-pokerpp-opt|g' > tmp.yaml && mv tmp.yaml "$yaml"
    done

    cd ../..
done
