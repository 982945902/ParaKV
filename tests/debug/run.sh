#!/bin/bash

clear

# python data_validation.py --host 127.0.0.1 --port 9200 --namespace default --count 1 --batch_size 1


python data_validation.py --mode write --fixed --count 600

# python data_validation.py --mode verify --fixed --count 600

# python data_validation.py --mode delete --fixed --count 600