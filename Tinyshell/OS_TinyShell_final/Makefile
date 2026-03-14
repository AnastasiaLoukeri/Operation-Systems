# Ορισμός Μεταβλητών (Variables)
CC = gcc
# CFLAGS: Σημαίες μεταγλωττιστή
CFLAGS = -Wall

# LIBS: Βιβλιοθήκη για σύνδεση: Απαραίτητο το -lreadline
LIBS = -lreadline

# Ονόματα Αρχείων
TARGET = tinyshell
SOURCE = tinyshell.c

# 1. Κανόνας 'all' (Ο βασικός στόχος)
all: $(TARGET)

# 2. Κανόνας Δημιουργίας Εκτελέσιμου
# Στόχος: tinyshell
# Εξάρτηση: tinyshell.c
$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LIBS)

# 3. Κανόνας 'clean' (για καθαρισμό του φακέλου)
# Εκτελείται με την εντολή 'make clean'
