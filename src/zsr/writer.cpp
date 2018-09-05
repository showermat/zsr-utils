#include "writer.h"

namespace zsr
{
	void writer::writestring(const std::string &s, std::ostream &out)
	{
		uint16_t len = s.size();
		serialize(out, len);
		serialize(out, s);
	}

	filecount writer::recursive_process(const std::string &path, filecount parent, std::ofstream &contout, std::ofstream &idxout) // TODO Needs a little refactoring
	{
		constexpr offset ptrfill{0};
		std::string fullpath = util::resolve(util::dirname(fullroot_), path); // TODO Inefficient
		struct stat s;
		filenode::ntype type = filenode::ntype::reg;
		DIR *dir = nullptr;
		std::ifstream in{};
		in.exceptions(std::ios_base::badbit);
		std::string fulltarget{};
		try
		{
			if (lstat(path.c_str(), &s) != 0) throw std::runtime_error{"Couldn't lstat " + path};
			if ((s.st_mode & S_IFMT) == S_IFLNK)
			{
				if (linkpol_ == linkpolicy::skip) return 0;
				fulltarget = util::linktarget(fullpath);
				struct stat ls;
				if (stat(path.c_str(), &ls) != 0) throw std::runtime_error{"Broken symbolic link " + path};
				if (util::is_under(fullroot_, fulltarget) && linkpol_ == linkpolicy::process) type = filenode::ntype::link; // Only treat as link if destination is inside the tree
				else s = ls;
			}
			if ((s.st_mode & S_IFMT) == S_IFDIR)
			{
				dir = opendir(path.c_str());
				if (! dir) throw std::runtime_error{"Couldn't open directory " + path};
				type = filenode::ntype::dir;
			}
			if ((s.st_mode & S_IFMT) == S_IFREG)
			{
				in.open(path);
				if (! in) throw std::runtime_error{"Couldn't open file " + path};
			}
		}
		catch (std::runtime_error &e)
		{
			std::cout << "\r\033[K" << e.what() << "\n";
			return 0;
		}
		filecount id = nfile_++;
		logb(id << " " << path);
		//std::cout << "File " << id << " path " << path << " parent " << parent << "\n";
		if (linkpol_ == linkpolicy::process) links_.handle_dest(fullpath, id);
		offset mypos = contout.tellp();
		serialize(idxout, mypos);
		serialize(contout, parent);
		serialize(contout, type);
		writestring(util::basename(path), contout);
		const std::vector<std::string> metad = metagen_(filenode(id, path, s));
		if (type == filenode::ntype::reg)
		{
			if (metad.size() != nodemeta_.size()) throw std::runtime_error{"Number of generated metadata does not match number of file metadata keys"};
			for (const std::string &val : metad) writestring(val, contout);
			serialize(contout, static_cast<offset>(util::fsize(path)));
			std::streampos sizepos = contout.tellp();
			serialize(contout, ptrfill); // Placeholder for length
			lzma::wrbuf compressor{in};
			contout << &compressor;
			offset len = static_cast<offset>(contout.tellp() - sizepos - sizeof(offset));
			std::streampos end = contout.tellp();
			contout.seekp(sizepos);
			serialize(contout, len);
			contout.seekp(end);
		}
		if (type == filenode::ntype::link)
		{
			links_.handle_src(fullpath, contout.tellp());
			constexpr filecount fcfill{0}; // To fill in later
			serialize(contout, fcfill);
		}
		if (type == filenode::ntype::dir)
		{
			diskmap::writer<std::string, filecount> children{};
			std::streampos childstart = contout.tellp();
			size_t maxnchild = 0;
			struct dirent *ent;
			while ((ent = readdir(dir))) if (! (linkpol_ == linkpolicy::skip && ent->d_type == DT_LNK)) maxnchild++;
			rewinddir(dir);
			maxnchild -= 2;
			contout.seekp(children.hdrsize + children.recsize * maxnchild, std::ios::cur);
			struct dirent *file;
			while ((file = readdir(dir)))
			{
				std::string fname{file->d_name};
				if (fname == "." || fname == "..") continue;
				filecount childid = recursive_process(path + util::pathsep + fname, id, contout, idxout);
				if (childid != 0) children.add(fname, childid);
			}
			if (closedir(dir)) throw std::runtime_error{"Couldn't close " + path + ": " + strerror(errno)};
			std::streampos end = contout.tellp();
			contout.seekp(childstart);
			children.write(contout);
			contout.seekp(end);
		}
		return id;
	}

	void linkmgr::add(const std::string &src, const std::string &dest)
	{
		links_.push_back(linkinfo{});
		linkinfo *inserted = &links_.back();
		by_src_.insert(std::make_pair(src, inserted));
		by_dest_.insert(std::make_pair(dest, inserted));
	}

	bool linkmgr::walk_add(const std::string &path, const struct stat *st, void *arg)
	{
		linkmgr *caller = (linkmgr *) arg;
		if ((st->st_mode & S_IFMT) == S_IFLNK)
		{
			std::string target = util::realpath(util::linktarget(path));
			if (! util::fexists(target)) return false;
			if (util::is_under(caller->root_, target)) caller->add(path, target);
			else return true;
			return false;
		}
		return true;
	}

	void linkmgr::search()
	{
		util::fswalk(root_, walk_add, (void *) this, false);
	}

	void linkmgr::handle_src(const std::string &path, std::streampos destpos)
	{
		std::unordered_map<std::string, linkinfo *>::iterator iter = by_src_.find(path);
		if (iter == by_src_.end()) throw std::runtime_error{"Couldn't find " + path + " in link table"};
		iter->second->destpos = destpos;
	}

	void linkmgr::handle_dest(const std::string &path, filecount id)
	{
		std::pair<std::unordered_map<std::string, linkinfo *>::iterator, std::unordered_map<std::string, linkinfo *>::iterator> range = by_dest_.equal_range(path);
		for (std::unordered_map<std::string, linkinfo *>::iterator iter = range.first; iter != range.second; iter++)
		{
			iter->second->destid = id;
			iter->second->resolved = true;
		}
	}

	void writer::node_meta(const std::vector<std::string> keys, std::function<std::unordered_map<std::string, std::string>(const filenode &)> generator)
	{
		nodemeta_ = keys;
		metagen_ = [keys, generator](const filenode &file) {
			std::unordered_map<std::string, std::string> metamap = generator(file);
			std::vector<std::string> ret{};
			for (const std::string &key : keys) ret.push_back(metamap.count(key) ? metamap.at(key) : "");
			return ret;
		};
	}

	void linkmgr::write(std::ostream &out)
	{
		size_t total = by_src_.size();
		size_t done = 0;
		for (const std::pair<const std::string, linkinfo *> &link : by_src_)
		{
			logb(++done << "/" << total);
			if (! link.second->resolved) throw std::runtime_error{"Link " + link.first + " was not resolved"};
			out.seekp(link.second->destpos);
			serialize(out, link.second->destid);
		}
	}

	void writer::write_body(const std::string &contname, const std::string &idxname)
	{
		if (linkpol_ == linkpolicy::process)
		{
			logb("Finding links");
			links_.search();
			loga(links_.size() << " links found");
		}
		loga("Writing archive body");
		contf_ = contname;
		idxf_ = idxname;
		std::ofstream contout{contname}, idxout{idxname};
		if (! contout) throw std::runtime_error{"Couldn't open " + contname + " for writing"};
		if (! idxout) throw std::runtime_error{"Couldn't open " + idxname + " for writing"};
		contout.exceptions(std::ios_base::badbit);
		idxout.exceptions(std::ios_base::badbit);
		nfile_ = 0;
		recursive_process(root_, 0, contout, idxout);
		loga("Wrote " << nfile_ << " entries");
		if (linkpol_ == linkpolicy::process)
		{
			loga("Writing links");
			links_.write(contout);
		}
	}

	void writer::write_header(const std::string &tmpfname)
	{
		headf_ = tmpfname;
		std::ofstream out{tmpfname};
		out.exceptions(std::ios_base::badbit);
		if (! out) throw std::runtime_error{"Couldn't open " + tmpfname + " for writing"};
		serialize(out, magic_number);
		serialize(out, version);
		serialize(out, std::string(sizeof(offset), '\0'));
		uint8_t msize = volmeta_.size();
		serialize(out, msize);
		for (const std::pair<const std::string, std::string> &pair : volmeta_)
		{
			writestring(pair.first, out);
			writestring(pair.second, out);
		}
		msize = nodemeta_.size();
		serialize(out, msize);
		for (const std::string &mkey : nodemeta_) writestring(mkey, out);
	}

	void writer::combine(std::ofstream &out)
	{
		loga("Combining archive components");
		if (! out) throw std::runtime_error{"Could not open archive output file"};
		std::ifstream header{headf_};
		std::ifstream content{contf_};
		std::ifstream index{idxf_};
		header.exceptions(std::ios_base::badbit);
		content.exceptions(std::ios_base::badbit);
		index.exceptions(std::ios_base::badbit);
		out << header.rdbuf();
		out << content.rdbuf();
		offset idxstart = static_cast<offset>(out.tellp());
		out.seekp(magic_number.size() + sizeof(version));
		serialize(out, idxstart); // FIXME Endianness problems?
		out.seekp(0, std::ios_base::end);
		serialize(out, nfile_);
		out << index.rdbuf();
		if (userdata_) out << userdata_->rdbuf();
		loga("Done writing archive");
		for (const std::string &file : {headf_, contf_, idxf_}) util::rm(file);
	}

	void writer::write(std::ofstream &out)
	{
		//std::function<void(int)> sighdl = [this](int sig) { for (const std::string &file : {headf_, contf_, idxf_}) util::rm(file); };
		//struct sigaction act, oldact;
		//act.sa_flags = 0;
		//::sigemptyset(&act.sa_mask);
		//act.sa_handler = *sighdl.target<void(*)(int)>();
		//if (! ::sigaction(SIGINT, &act, &oldact));
		write_header();
		write_body();
		combine(out);
		//::sigaction(SIGINT, &oldact, nullptr);
	}

	writer::~writer()
	{
		for (const std::string &file : {headf_, contf_, idxf_}) if (util::fexists(file)) util::rm(file);
	}
}
