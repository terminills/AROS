# Copyright (C) 2026, The AROS Development Team. All rights reserved.
CXX ?= g++
CXXFLAGS ?= -O2 -DNDEBUG
CPPFLAGS += -I$(SOURCE)/../Moira
OBJECTS = host_test.o moveq_cache.o Moira.o MoiraDebugger.o
all: moveq-test
moveq-test: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@
host_test.o: $(SOURCE)/host_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -c $< -o $@
moveq_cache.o: $(SOURCE)/moveq_cache.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -c $< -o $@
Moira.o: $(SOURCE)/../Moira/Moira/Moira.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -c $< -o $@
MoiraDebugger.o: $(SOURCE)/../Moira/Moira/MoiraDebugger.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -c $< -o $@
-include $(OBJECTS:.o=.d)
