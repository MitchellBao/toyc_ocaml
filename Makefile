CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -pipe -Wall -Wextra -Isrc

ifeq ($(OS),Windows_NT)
EXEEXT ?= .exe
else
EXEEXT ?=
endif

TARGET := compiler$(EXEEXT)
SOURCES := \
	src/main.cpp \
	src/lexer.cpp \
	src/parser.cpp \
	src/semantic.cpp \
	src/llvm_codegen.cpp

HEADERS := \
	src/ast.h \
	src/lexer.h \
	src/parser.h \
	src/semantic.h \
	src/llvm_codegen.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

clean:
	$(RM) $(TARGET)
