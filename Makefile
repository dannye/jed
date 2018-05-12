all:
	@mkdir build -p
	g++ --std=c++11 -o build/encoder src/encoder.cpp
	g++ --std=c++11 -o build/decoder src/decoder.cpp

clean:
	rm build/encoder build/decoder
