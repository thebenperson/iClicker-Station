libs := SoapySDR

CXXFLAGS := -Wall -Wextra -Werror -Wpedantic
CXXFLAGS += $(shell pkgconf --cflags $(libs))

LDFLAGS :=
LDLIBS  := $(shell pkgconf --libs $(libs))

all: debug

debug: FORCE
debug: CXXFLAGS += -DDEBUG -O0 -g
debug: LDFLAGS  += -fsanitize=address
debug: bin/a.out

release: FORCE
release: CXXFLAGS += -O3
release: LDFLAGS  += -s
release: bin/a.out

objects := $(patsubst src/%.cc,bin/%.o,$(wildcard src/*.cc))

bin/a.out: $(objects)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

bin/%.o: src/%.cc
	$(CXX) -c $(CXXFLAGS) $^ -o $@

clean: FORCE
	rm -r $(wildcard bin/*)

FORCE:
