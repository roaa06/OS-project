build:
	gcc process_generator.c -o process_generator.out
	gcc clk.c -o clk.out
	gcc scheduler.c -o scheduler.out -lm
	gcc process.c -o process.out
	gcc test_generator.c -o test_generator.out

clean:
	rm -f *.out scheduler.log scheduler.perf

all: clean build

run:
	./process_generator.out

test-sjf: build
	./run_tests.sh sjf

test-rr: build
	./run_tests.sh rr

test-hpf: build
	./run_tests.sh hpf

test-bonus: build
	./run_tests.sh bonus

test-all: build
	./run_tests.sh all
