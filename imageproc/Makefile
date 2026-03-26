CC     = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -D_POSIX_C_SOURCE=200809L
SRCS   = imageproc.c ppm.c kmeans.c parent.c worker.c tile.c enhance.c
OBJS   = $(SRCS:.c=.o)
TARGET = imageproc

# Test binaries
TEST_PPM     = tests/test_ppm
TEST_KMEANS  = tests/test_kmeans
TEST_ENHANCE = tests/test_enhance

.PHONY: all clean test tests valgrind

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- unit test builds ----
$(TEST_PPM): tests/test_ppm.c ppm.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(TEST_KMEANS): tests/test_kmeans.c kmeans.o ppm.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(TEST_ENHANCE): tests/test_enhance.c enhance.o ppm.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

tests: $(TEST_PPM) $(TEST_KMEANS) $(TEST_ENHANCE)
	@echo "=== test_ppm ==="
	./$(TEST_PPM)
	@echo "=== test_kmeans ==="
	./$(TEST_KMEANS)
	@echo "=== test_enhance ==="
	./$(TEST_ENHANCE)

# ---- integration test ----
test: $(TARGET)
	./$(TARGET) tests/beatutifulTuotuo.ppm /tmp/out.ppm 3 2
	@test -f /tmp/out.ppm && echo "PASS: output file exists" || echo "FAIL: no output"
	@cmp -s tests/beatutifulTuotuo.ppm /tmp/out.ppm \
		&& echo "FAIL: output identical to input" \
		|| echo "PASS: output differs from input"

valgrind: $(TARGET)
	valgrind --leak-check=full --track-fds=yes \
		./$(TARGET) tests/beatutifulTuotuo.ppm /tmp/val_out.ppm 3 2

clean:
	rm -f $(OBJS) $(TARGET) \
	      $(TEST_PPM) $(TEST_KMEANS) $(TEST_ENHANCE) \
	      tests/*.o \
	      /tmp/imageproc_*.ppm
