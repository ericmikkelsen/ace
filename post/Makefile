CFLAGS=-O3 -g -fPIC
#CFLAGS=-g
LD?=ld
OBJS = train.o words.o tags.o hmm.o

ifeq ($(WASM),1)
include ../WASM.config
else ifeq ($(shell uname -s),Darwin)
include ../MacOSX.config
# post.o is built with `ld -r`, which can't read LTO bitcode
override LTO=
endif

# compilation flags:

CFLAGS += -DUSE_HMM
# uncomment the above line and recompile
# to enable bigram HMM tagging.

#CFLAGS += -DNO_MORPH_RULES
# uncomment the above line and recompile
# to disable rules for classifying unknown words.
# in this case, all unknown words will be tagged as NN.

post.o: ${OBJS}
	${LD} -r -o post.o ${OBJS}

clean:
	rm -f ${OBJS} post.o
