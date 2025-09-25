#!/bin/bash

IMAGE_LIST="docker-images.txt"

while IFS= read -r image; do
  [ -z "$image" ] && continue
  echo "Pulling image: $image"
  crictl pull "$image"
done < "$IMAGE_LIST"
