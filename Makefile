all: oss user

oss: oss.c
	gcc -lrt -o oss oss.c
	
user: user.c
	gcc -o user user.c
	
clean:
	rm oss user *.log