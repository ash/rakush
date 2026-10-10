# Build rakush against a rakupp checkout built with cmake (its static archives).
#   make                      # expects ~/raku++ with ~/raku++/build
#   make RAKUPP=/path/to/rakupp
RAKUPP   ?= $(HOME)/raku++
BUILD    ?= $(RAKUPP)/build
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall

ARCHIVES = $(BUILD)/librakupp_rt.a $(BUILD)/librakupp_parse.a \
           $(BUILD)/librakupp_ucd_names.a $(BUILD)/librakupp_ucd_coll.a \
           $(BUILD)/librakupp_ucd_props.a

ifeq ($(shell uname),Darwin)
  LINK = $(ARCHIVES) -Wl,-dead_strip
  LIBS = -liconv -ledit
else
  LINK = -Wl,--start-group $(ARCHIVES) -Wl,--end-group -Wl,--gc-sections
  LIBS = -ledit -lpthread -ldl
endif

rakush: rakush.cpp $(ARCHIVES)
	$(CXX) $(CXXFLAGS) -I$(RAKUPP)/include rakush.cpp $(LINK) $(LIBS) -o $@

test: rakush
	sh t/run.sh ./rakush

clean:
	rm -f rakush

.PHONY: test clean
