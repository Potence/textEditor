editor: editor.c
	gcc editor.c -o editor -Wall -Wextra -pedantic -std=c99

debug: editor.c
	gcc -g editor.c -o editor -Wall -Wextra -pedantic -std=c99

