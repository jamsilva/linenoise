tcc -D__WIN32 -c libline.c -o libline.o
tcc -D__WIN32 -c main.o -o main.o
tcc main.o libline.o -o main.exe