SRC_DIR := src
OBJ_DIR := obj

SRC := $(wildcard $(SRC_DIR)/*.c)
OBJ := $(OBJ_DIR)/y.tab.o $(OBJ_DIR)/lex.yy.o $(OBJ_DIR)/parse.o $(OBJ_DIR)/example.o
BIN := example liso_server echo_client

CC      := gcc
CPPFLAGS := -Iinclude
CFLAGS   := -g -Wall

default: all

all: example liso_server echo_client

example: $(OBJ)
	$(CC) $^ -o $@

$(SRC_DIR)/lex.yy.c: $(SRC_DIR)/lexer.l
	flex -o $@ $^

$(SRC_DIR)/y.tab.c $(SRC_DIR)/y.tab.h: $(SRC_DIR)/parser.y
	bison -d -o $(SRC_DIR)/y.tab.c $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# 第二阶段：服务端可执行文件名应为 liso_server
liso_server: $(OBJ_DIR)/echo_server.o $(OBJ_DIR)/y.tab.o $(OBJ_DIR)/lex.yy.o $(OBJ_DIR)/parse.o
	$(CC) -Werror $^ -o $@

# 兼容第一阶段命名（可选）
echo_server: liso_server
	ln -sf liso_server echo_server

echo_client: $(OBJ_DIR)/echo_client.o
	$(CC) -Werror $^ -o $@

$(OBJ_DIR):
	mkdir -p $@

clean:
	$(RM) $(OBJ) $(BIN) $(SRC_DIR)/lex.yy.c $(SRC_DIR)/y.tab.*
	$(RM) -r $(OBJ_DIR)

.PHONY: all clean