#include "compress.h"

namespace lzma
{
	const int chunksize = 2 * 1024 * 1024;
	static lzma_options_lzma options{LZMA_DICT_SIZE_DEFAULT, nullptr, 0, LZMA_LC_DEFAULT, LZMA_LP_DEFAULT, LZMA_PB_DEFAULT, LZMA_MODE_NORMAL, 128, LZMA_MF_HC4, 0};
	static lzma_filter default_filters[2] = {{LZMA_FILTER_LZMA2, &options}, {LZMA_VLI_UNKNOWN}};

	void code(lzma_stream &stream, std::istream &in, std::ostream &out)
	{
		std::vector<char> inbuf(chunksize, 0), outbuf(chunksize, 0);
		while (true)
		{
			stream.next_in = reinterpret_cast<uint8_t *>(&inbuf[0]);
			in.read(&inbuf[0], inbuf.size());
			std::streamsize insize = in.gcount();
			stream.avail_in = insize;
			stream.total_in = 0;
			do
			{
				stream.total_out = 0;
				stream.next_out = reinterpret_cast<uint8_t *>(&outbuf[0]);
				stream.avail_out = outbuf.size();
				lzma_ret retval = lzma_code(&stream, insize == 0 ? LZMA_FINISH : LZMA_RUN);
				if (retval == LZMA_STREAM_END)
				{
					if (stream.avail_in != 0) throw compress_error{"Unprocessed input remaining in stream"};
					out.write(&outbuf[0], stream.total_out);
					lzma_end(&stream);
					return;
				}
				if (retval != LZMA_OK) throw compress_error{"Stream encoding failed"};
				//std::cout << "Retrieved " << stream.total_in << " bytes and output " << stream.total_out << " bytes\n";
				out.write(&outbuf[0], stream.total_out);
			}
			while (stream.total_out > 0);
		}
	}
	
	void compress(std::istream &in, std::ostream &out)
	{
		lzma_stream stream = LZMA_STREAM_INIT;
		if (lzma_raw_encoder(&stream, default_filters) != LZMA_OK) throw compress_error{"Stream encoder setup failed"};
		code(stream, in, out);
	}

	void decompress(std::istream &in, std::ostream &out)
	{
		lzma_stream stream = LZMA_STREAM_INIT;
		if (lzma_raw_decoder(&stream, default_filters) != LZMA_OK) throw compress_error{"Stream decoder setup failed"};
		code(stream, in, out);
	}

	void buf_base::init(std::istream &file)
	{
		file_ = &file;
		buf_.resize(chunksize);
		inbuf_.resize(chunksize);
		action_ = LZMA_RUN;
		lzma_ = LZMA_STREAM_INIT;
		pos_ = 0;
		char *begin = &buf_[0];
		setg(begin, begin, begin);
	}

	buf_base::buf_base(buf_base &&orig) : file_{orig.file_}, lzma_{orig.lzma_}, buf_{std::move(orig.buf_)}, inbuf_{std::move(orig.inbuf_)}, action_{orig.action_}, pos_{orig.pos_}
	{
		orig.lzma_.internal = nullptr;
	}

	std::streamsize buf_base::load()
	{
		lzma_.next_out = reinterpret_cast<uint8_t *>(&buf_[0]);
		if (lzma_.total_out > 0)
		{
			lzma_.avail_out = buf_.size();
			lzma_.total_out = 0;
			lzma_ret retval = lzma_code(&lzma_, action_);
			if (retval != LZMA_OK && retval != LZMA_STREAM_END) throw compress_error{"Compression failed with liblzma error " + util::t2s(retval)};
			if (lzma_.total_out > 0) goto loaded; // Valid use case?
		}
		lzma_.next_out = reinterpret_cast<uint8_t *>(&buf_[0]);
		while (lzma_.total_out == 0)
		{
			lzma_.total_in = 0;
			lzma_.next_in = reinterpret_cast<uint8_t *>(&inbuf_[0]);
			std::streamsize insize = fill();
			if (insize == 0) action_ = LZMA_FINISH;
			lzma_.avail_in = insize;
			lzma_.avail_out = buf_.size();
			lzma_ret retval = lzma_code(&lzma_, action_);
			//std::cout << "Got " << lzma_.total_in << " bytes, compressed to " << lzma_.total_out << "\n";
			if (retval == LZMA_STREAM_END) break;
			else if (retval != LZMA_OK) throw compress_error{"Compression failed with liblzma error " + util::t2s(retval)};
		}
	loaded:	char *start = &buf_[0], *end = start + lzma_.total_out;
		pos_ += egptr() - eback();
		setg(start, start, end);
		return lzma_.total_out;
	}
	//catch (std::exception &e) { std::cerr << "Error: " << e.what() << "\n"; return 0; } // FIXME Exceptions aren't propagated by operator <<.  Use ios::exceptions flags instead?
	
	std::streambuf::int_type buf_base::underflow()
	{
		std::streamsize loadsize = load();
		if (gptr() >= egptr() && loadsize == 0) return traits_type::eof();
		//std::cout << "\033[31mLoaded " << loadsize << " bytes, position " << pos_ << "\033[m\n";
		return traits_type::to_int_type(*gptr());
	}

	void wrbuf::init(std::istream &file)
	{
		buf_base::init(file);
		if (lzma_raw_encoder(&lzma_, default_filters) != LZMA_OK) throw compress_error{"Stream encoder setup failed"};
	}

	std::streamsize wrbuf::fill()
	{
		file_->read(&inbuf_[0], inbuf_.size());
		return file_->gcount();
	}
	
	std::streambuf::pos_type rdbuf::seekpos(pos_type target, std::ios_base::openmode which)
	{
		//std::cerr << ">> Absolute seek to " << target << "\n";
		if (target < pos_) reset(); // TODO Optimize by checking if the seek destination is still in the get buffer
		while (pos_ + pos_type(egptr() - eback()) < target)
		{
			//int_type loadsize = underflow();
			if (underflow() == traits_type::eof()) return pos_ + egptr() - gptr();
		}
		setg(eback(), eback() + target - pos_, egptr());
		return target;
	}

	std::streambuf::pos_type rdbuf::seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
	{
		if (dir == std::ios_base::beg) seekpos(pos_type(off), which);
		else if (dir == std::ios_base::cur) seekpos(pos_ + pos_type(gptr() - eback() + off), which);
		else if (dir == std::ios_base::end) seekpos(decomp_ + pos_type(off), which);
		else return pos_type(-1);
		return pos_ + gptr() - eback();
	}

	void rdbuf::init(std::istream &file, std::streampos start, std::streampos size, std::streampos decomp)
	{
		buf_base::init(file);
		ptr_ = 0;
		start_ = start;
		size_ = size;
		decomp_ = decomp;
		if (lzma_raw_decoder(&lzma_, default_filters) != LZMA_OK) throw compress_error{"Stream decoder setup failed"};
	}

	std::streamsize rdbuf::fill()
	{
		file_->seekg(start_ + ptr_);
		file_->read(&inbuf_[0], static_cast<std::streamsize>(inbuf_.size()) < size_ - ptr_ ? inbuf_.size() : size_ - ptr_);
		std::streamsize insize = file_->gcount();
		ptr_ += insize;
		return insize;
	}

	rdbuf::rdbuf(std::istream &file, std::streampos start, std::streampos size, std::streampos decomp) : buf_base{}
	{
		init(file, start, size, decomp);
	}

	memrdbuf::memrdbuf(const char *source, std::streampos size, std::streampos start, std::streampos len) : source_{source}, buf_{},
		start_{start}, size_{size}, len_{len}
	{
		buf_.resize(chunksize);
		reset();
	}

	memrdbuf::pos_type memrdbuf::ff_to(pos_type target)
	{
		while (pos_ + egptr() - eback() < target) if (underflow() == traits_type::eof()) return pos_ + egptr() - gptr();
		setg(eback(), eback() + target - pos_, egptr());
		return target;
	}

	void memrdbuf::reset()
	{
		action_ = LZMA_RUN;
		lzma_ = LZMA_STREAM_INIT;
		if (lzma_raw_decoder(&lzma_, default_filters) != LZMA_OK) throw compress_error{"Stream decoder setup failed"};
		pos_ = 0;
		char *begin = &buf_[0];
		setg(begin, begin, begin);
		lzma_.next_in = reinterpret_cast<const uint8_t*>(source_);
		lzma_.avail_in = size_;
		ff_to(start_);
	}

	std::streamsize memrdbuf::load()
	{
		lzma_.next_out = reinterpret_cast<uint8_t *>(&buf_[0]);
		if (lzma_.total_out > 0)
		{
			lzma_.avail_out = buf_.size();
			lzma_.total_out = 0;
			lzma_ret retval = lzma_code(&lzma_, action_);
			if (retval != LZMA_OK && retval != LZMA_STREAM_END) throw compress_error{"Compression failed with liblzma error " + util::t2s(retval)};
			if (lzma_.total_out > 0) goto loaded;
		}
		lzma_.next_out = reinterpret_cast<uint8_t *>(&buf_[0]);
		while (lzma_.total_out == 0)
		{
			if (lzma_.avail_in == 0) action_ = LZMA_FINISH;
			lzma_.avail_out = buf_.size();
			lzma_ret retval = lzma_code(&lzma_, action_);
			if (retval == LZMA_STREAM_END) break;
			else if (retval != LZMA_OK) throw compress_error{"Compression failed with liblzma error " + util::t2s(retval)};
		}
	loaded:	char *start = &buf_[0], *end = start + lzma_.total_out;
		pos_ += egptr() - eback();
		setg(start, start, end);
		return lzma_.total_out;
	}

	memrdbuf::int_type memrdbuf::underflow()
	{
		std::streamsize loadsize = load();
		if (pos_ + egptr() - eback() > start_ + len_) setg(eback(), gptr(), eback() + start_ + len_ - pos_);
		if (gptr() >= egptr() && (pos_ + egptr() - eback() >= start_ + len_ || loadsize == 0)) return traits_type::eof();
		return traits_type::to_int_type(*gptr());
	}

	memrdbuf::pos_type memrdbuf::seekpos(pos_type target, std::ios_base::openmode which)
	{
		if (target >= len_) target = len_ - pos_type(1);
		target += start_;
		if (target < pos_) reset(); // TODO Optimize by checking if the seek destination is still in the get buffer
		return ff_to(target) - start_;
	}

	memrdbuf::pos_type memrdbuf::seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
	{
		if (dir == std::ios_base::beg) seekpos(off, which);
		else if (dir == std::ios_base::cur) seekpos(pos_ - start_ + gptr() - eback() + off, which);
		else if (dir == std::ios_base::end) seekpos(len_ + off, which);
		else return -1;
		return pos_ + gptr() - eback() - start_;
	}
}