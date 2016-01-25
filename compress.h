#include <streambuf>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <lzma.h>
#include "util.h"

namespace lzma
{
	const int compression = 6;
	const int memlimit = 1 * 1024 * 1024 * 1024;
	const int chunksize = 32 * 1024;

	void compress(std::istream &in, std::ostream &out);

	void decompress(std::istream &in, std::ostream &out);

	class compress_error : public std::runtime_error
	{
	public:
		compress_error(std::string msg) : runtime_error{msg} { }
	};

	class buf_base : public std::streambuf
	{
	protected:
		std::istream *file_;
		lzma_stream lzma_;
		std::vector<char> buf_, inbuf_;
		lzma_action action_;
		virtual std::streamsize fill() = 0;
		virtual std::streamsize load();
	public:
		buf_base() : file_{}, lzma_{}, buf_{} { }
		buf_base(const buf_base &orig) = delete;
		buf_base(buf_base &&orig);
		virtual void init(std::istream &file);
		virtual void reset() = 0;
		int_type underflow();
		virtual ~buf_base() { if (lzma_.internal) lzma_end(&lzma_); }
	};

	class wrbuf : public buf_base
	{
	private:
		std::streamsize fill();
	public:
		wrbuf() : buf_base{} { };
		wrbuf(std::istream &file) : buf_base{} { init(file); }
		wrbuf(const wrbuf &orig) = delete;
		wrbuf(wrbuf &&orig) : buf_base{std::move(orig)} { }
		void init(std::istream &file);
		void reset() { init(*file_); }
	};

	class rdbuf : public buf_base
	{
	private:
		std::streampos ptr_, start_, size_;
		std::streamsize fill();
	public:
		rdbuf() : buf_base{}, ptr_{}, start_{}, size_{} { };
		rdbuf(std::istream &file, std::streampos start, std::streampos size);
		rdbuf(const rdbuf &orig) = delete;
		rdbuf(rdbuf &&orig) : buf_base{std::move(orig)}, ptr_{orig.ptr_}, start_{orig.start_}, size_{orig.size_} { }
		void init(std::istream &file, std::streampos start, std::streampos size);
		void reset() { init(*file_, start_, size_); }
	};
}
