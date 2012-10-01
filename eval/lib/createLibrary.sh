echo $PWD
find ../build -name *.so -exec ln -s {} \;
# export LD_LIBRARY_PATH=./lib
