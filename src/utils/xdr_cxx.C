// The libMesh Finite Element Library.
// Copyright (C) 2002-2012 Benjamin S. Kirk, John W. Peterson, Roy H. Stogner

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


// C/C++ includes
#include <cstring>
#include <limits>

// Local includes
#include "libmesh/xdr_cxx.h"
#include "libmesh/libmesh_logging.h"
#include "libmesh/o_f_stream.h"
#include "libmesh/o_string_stream.h"
#ifdef LIBMESH_HAVE_GZSTREAM
# include "gzstream.h"
#endif


// Anonymous namespace for implementation details.
namespace {

  // Nasty hacks for reading/writing zipped files
  void bzip_file (const std::string &unzipped_name)
  {
    // There's no parallel bzip2 for us to call, but we might
    // be running multiple zip utilities on parallel files.
    // libmesh_assert_equal_to (libMesh::processor_id(), 0);

#ifdef LIBMESH_HAVE_BZIP
    START_LOG("system(bzip2)", "XdrIO");

    std::string system_string = "bzip2 -f ";
    system_string += unzipped_name;
    if (std::system(system_string.c_str()))
      libmesh_file_error(system_string);

    STOP_LOG("system(bzip2)", "XdrIO");
#else
    libMesh::err << "ERROR: need bzip2/bunzip2 to create "
		 << unzipped_name << ".bz2"
	         << std::endl;
    libmesh_error();
#endif
  }

  std::string unzip_file (const std::string &name)
  {
    // There's no parallel bunzip2 or xz for us to call, but we might
    // be running multiple zip utilities on parallel files.
    // libmesh_assert_equal_to (libMesh::processor_id(), 0);

    OStringStream pid_suffix;
    pid_suffix << '_' << getpid();

    std::string new_name = name;
    if (name.size() - name.rfind(".bz2") == 4)
      {
#ifdef LIBMESH_HAVE_BZIP
	new_name.erase(new_name.end() - 4, new_name.end());
        new_name += pid_suffix.str();
	START_LOG("system(bunzip2)", "XdrIO");
	std::string system_string = "bunzip2 -f -k -c ";
	system_string += name + " > " + new_name;
	if (std::system(system_string.c_str()))
          libmesh_file_error(system_string);
	STOP_LOG("system(bunzip2)", "XdrIO");
#else
        libMesh::err << "ERROR: need bzip2/bunzip2 to open .bz2 file "
		     << name << std::endl;
        libmesh_error();
#endif
      }
    else if (name.size() - name.rfind(".xz") == 3)
      {
#ifdef LIBMESH_HAVE_XZ
	new_name.erase(new_name.end() - 3, new_name.end());
        new_name += pid_suffix.str();
	START_LOG("system(xz -d)", "XdrIO");
	std::string system_string = "xz -f -d -k -c ";
	system_string += name + " > " + new_name;
	if (std::system(system_string.c_str()))
          libmesh_file_error(system_string);
	STOP_LOG("system(xz -d)", "XdrIO");
#else
        libMesh::err << "ERROR: need xz to open .xz file "
		     << name << std::endl;
        libmesh_error();
#endif
      }
    return new_name;
  }

  void xzip_file (const std::string &unzipped_name)
  {
    // There's no parallel xz for us to call, but we might
    // be running multiple zip utilities on parallel files.
    // libmesh_assert_equal_to (libMesh::processor_id(), 0);

#ifdef LIBMESH_HAVE_XZ
    START_LOG("system(xz)", "XdrIO");

    std::string system_string = "xz -f ";
    system_string += unzipped_name;
    if (std::system(system_string.c_str()))
      libmesh_file_error(system_string);

    STOP_LOG("system(xz)", "XdrIO");
#else
    libMesh::err << "ERROR: need xz to create "
		 << unzipped_name << ".xz"
	         << std::endl;
    libmesh_error();
#endif
  }


  // remove an unzipped file
  void remove_unzipped_file (const std::string &name)
  {
    OStringStream pid_suffix;
    pid_suffix << '_' << getpid();

    // If we temporarily decompressed a file, remove the
    // uncompressed version
    if (name.size() - name.rfind(".bz2") == 4)
      {
	std::string new_name(name.begin(), name.end()-4);
        new_name += pid_suffix.str();
	std::remove(new_name.c_str());
      }
    if (name.size() - name.rfind(".xz") == 3)
      {
	std::string new_name(name.begin(), name.end()-3);
        new_name += pid_suffix.str();
	std::remove(new_name.c_str());
      }
  }
}

namespace libMesh
{

//-------------------------------------------------------------
// Xdr class implementation
Xdr::Xdr (const std::string& name, const XdrMODE m) :
  mode(m),
  file_name(name),
#ifdef LIBMESH_HAVE_XDR
  xdrs(NULL),
  fp(NULL),
#endif
  in(NULL),
  out(NULL),
  comm_len(xdr_MAX_STRING_LENGTH),
  gzipped_file(false),
  bzipped_file(false),
  xzipped_file(false)
{
  this->open(name);
}



Xdr::~Xdr()
{
  this->close();
}



void Xdr::open (const std::string& name)
{
  file_name = name;

  if (name == "")
    return;

  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	fp = fopen(name.c_str(), (mode == ENCODE) ? "w" : "r");
        if (!fp)
          libmesh_file_error(name.c_str());
	xdrs = new XDR;
	xdrstdio_create (xdrs, fp, (mode == ENCODE) ? XDR_ENCODE : XDR_DECODE);
#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;

      }

    case READ:
      {
	gzipped_file = (name.size() - name.rfind(".gz")  == 3);
	bzipped_file = (name.size() - name.rfind(".bz2") == 4);
	xzipped_file = (name.size() - name.rfind(".xz") == 3);

	if (gzipped_file)
	  {
#ifdef LIBMESH_HAVE_GZSTREAM
	    igzstream *inf = new igzstream;
	    libmesh_assert(inf);
	    in.reset(inf);
	    inf->open(name.c_str(), std::ios::in);
#else
	    libMesh::err << "ERROR: need gzstream to handle .gz files!!!"
		          << std::endl;
	    libmesh_error();
#endif
	  }
	else
	  {
	    std::ifstream *inf = new std::ifstream;
	    libmesh_assert(inf);
	    in.reset(inf);

	    std::string new_name = unzip_file(name);

	    inf->open(new_name.c_str(), std::ios::in);
	  }

	libmesh_assert(in.get());
        libmesh_assert (in->good());
	return;
      }

    case WRITE:
      {
	gzipped_file = (name.size() - name.rfind(".gz")  == 3);
	bzipped_file = (name.size() - name.rfind(".bz2") == 4);
	xzipped_file = (name.size() - name.rfind(".xz")  == 3);

	if (gzipped_file)
	  {
#ifdef LIBMESH_HAVE_GZSTREAM
	    ogzstream *outf = new ogzstream;
	    libmesh_assert(outf);
	    out.reset(outf);
	    outf->open(name.c_str(), std::ios::out);
#else
	    libMesh::err << "ERROR: need gzstream to handle .gz files!!!"
		          << std::endl;
	    libmesh_error();
#endif
	  }
	else
	  {
	    std::ofstream *outf = new std::ofstream;
	    libmesh_assert(outf);
	    out.reset(outf);

	    std::string new_name = name;

	    if (bzipped_file)
	      new_name.erase(new_name.end() - 4, new_name.end());

	    if (xzipped_file)
	      new_name.erase(new_name.end() - 3, new_name.end());

	    outf->open(new_name.c_str(), std::ios::out);
	  }

	libmesh_assert(out.get());
        libmesh_assert (out->good());
	return;
      }

    default:
      libmesh_error();
    }
}



void Xdr::close ()
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	if (xdrs)
	  {
	    xdr_destroy (xdrs);
	    delete xdrs;
	    xdrs = NULL;
	  }

	if (fp)
	  {
	    fflush(fp);
	    fclose(fp);
	    fp = NULL;
	  }
#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	file_name = "";
	return;
      }

    case READ:
      {
	if (in.get() != NULL)
	  {
	    in.reset();

	    if (bzipped_file || xzipped_file)
	      remove_unzipped_file(file_name);
	  }
	file_name = "";
	return;
      }

    case WRITE:
      {
	if (out.get() != NULL)
	  {
	    out.reset();

	    if (bzipped_file)
	      bzip_file(std::string(file_name.begin(), file_name.end()-4));

	    else if (xzipped_file)
	      xzip_file(std::string(file_name.begin(), file_name.end()-3));
	  }
	file_name = "";
	return;
      }

    default:
      libmesh_error();
    }
}



bool Xdr::is_open() const
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	if (fp)
	  if (xdrs)
	    return true;

	return false;

#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

	return false;

#endif

      }

    case READ:
      {
	if (in.get() != NULL)
	  return in->good();
	return false;
      }

    case WRITE:
      {
	if (out.get() != NULL)
	  return out->good();
	return false;
      }

    default:
      libmesh_error();
    }

  return false;
}


#ifdef LIBMESH_HAVE_XDR

// Anonymous namespace for Xdr::data helper functions
namespace
{

template <typename T>
xdrproc_t xdr_translator();

template <typename T>
bool xdr_translate(XDR* x, T& a) {return (xdr_translator<T>())(x, &a); }

template <>
bool xdr_translate(XDR* x, std::string& s) {
  char* sptr = new char[xdr_MAX_STRING_LENGTH+1];
  std::copy(s.begin(), s.end(), sptr);
  sptr[s.size()] = 0;
  unsigned int length = xdr_MAX_STRING_LENGTH;
  bool b = xdr_string(x, &sptr, length);

  // This is necessary when reading, but inefficient when writing...
  length = std::strlen(sptr);
  s.resize(length);
  std::copy(sptr, sptr+length, s.begin());

  delete [] sptr;
  return b;
}

template <typename T>
bool xdr_translate(XDR* x, std::complex<T>& a) {
  T r = a.real(), i = a.imag();
  bool b1 = xdr_translate(x, r);
  bool b2 = xdr_translate(x, i);
  a = std::complex<T>(r,i);
  return (b1 && b2);
}

template <typename T>
bool xdr_translate(XDR* x, std::vector<T>& a) {
  unsigned int length = a.size();
  xdr_u_int(x, &length);
  if (length > 0)
  {
    a.resize(length);
    return xdr_vector(x, (char*) &a[0], length, sizeof(T),
                      xdr_translator<T>());
  }
  else
    return true;
}

template <typename T>
bool xdr_translate(XDR* x, std::vector<std::complex<T> >& a) {
  unsigned int length = a.size();
  bool b = xdr_u_int(x, &length);
  a.resize(length);
  typename std::vector<std::complex<T> >::iterator iter = a.begin();
  for (; iter != a.end(); ++iter)
    if (!xdr_translate(x, *iter))
      b = false;
  return b;
}

template <>
xdrproc_t xdr_translator<int>() { return (xdrproc_t)(xdr_int); }

template <>
xdrproc_t xdr_translator<unsigned int>() { return (xdrproc_t)(xdr_u_int); }

template <>
xdrproc_t xdr_translator<short int>() { return (xdrproc_t)(xdr_short); }

template <>
xdrproc_t xdr_translator<unsigned short int>() { return (xdrproc_t)(xdr_u_short); }

template <>
xdrproc_t xdr_translator<float>() { return (xdrproc_t)(xdr_float); }

template <>
xdrproc_t xdr_translator<double>() { return (xdrproc_t)(xdr_double); }

// FIXME - no XDR love for long doubles?
template <>
xdrproc_t xdr_translator<long double>() { return (xdrproc_t)(xdr_double); }

} // end anonymous namespace

#endif

template <typename T>
void Xdr::do_read(T& a) {
  *in >> a;
  in->getline(comm, comm_len);
}

template <typename T>
void Xdr::do_read(std::complex<T>& a) {
  T r, i;
  *in >> r >> i;
  a = std::complex<T>(r,i);
  in->getline(comm, comm_len);
}

template <>
void Xdr::do_read(std::string& a) {
  in->getline(comm, comm_len);

  a = "";

  for (unsigned int c=0; c<std::strlen(comm); c++)
    {
      if (comm[c] == '\t')
        break;
      a.push_back(comm[c]);
    }
}

template <typename T>
void Xdr::do_read(std::vector<T>& a) {
  unsigned int length=0;
  data(length, "# vector length");
  a.resize(length);

  for (unsigned int i=0; i<a.size(); i++)
    {
      libmesh_assert(in.get());
      libmesh_assert (in->good());
      *in >> a[i];
    }
  in->getline(comm, comm_len);
}

template <typename T>
void Xdr::do_read(std::vector<std::complex<T> >& a) {
  unsigned int length=0;
  data(length, "# vector length x 2 (complex)");
  a.resize(length);

  for (unsigned int i=0; i<a.size(); i++)
    {
      T r, im;
      libmesh_assert(in.get());
      libmesh_assert (in->good());
      *in >> r >> im;
      a[i] = std::complex<T>(r,im);
    }
  in->getline(comm, comm_len);
}

template <typename T>
void Xdr::do_write(T& a) { *out << a; }

template <typename T>
void Xdr::do_write(std::complex<T>& a) {
  *out << a.real() << "\t " << a.imag();
}

template <typename T>
void Xdr::do_write(std::vector<T>& a) {
  unsigned int length=a.size();
  data(length, "# vector length");

  for (unsigned int i=0; i<a.size(); i++)
    {
      libmesh_assert(out.get());
      libmesh_assert (out->good());
      this->do_write(a[i]);
      *out << "\t ";
    }
}

template <typename T>
void Xdr::do_write(std::vector<std::complex<T> >& a) {
  unsigned int length=a.size();
  data(length, "# vector length x 2 (complex)");

  for (unsigned int i=0; i<a.size(); i++)
    {
      libmesh_assert(out.get());
      libmesh_assert (out->good());
      this->do_write(a[i]);
      *out << "\t ";
    }
}



template <typename T>
void Xdr::data (T& a, const char* comment)
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (is_open());

	xdr_translate(xdrs, a);

#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());

	this->do_read(a);

	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());

	this->do_write(a);
        *out << "\t " << comment << '\n';

	return;
      }

    default:
      libmesh_error();
    }
}


template <typename T>
void Xdr::data_stream (T *val, const unsigned int len, const unsigned int line_break)
{
  switch (mode)
    {
    case ENCODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (this->is_open());


	xdr_vector(xdrs,
		   (char*) val,
		   len,
		   sizeof(unsigned int),
		   (xdrproc_t) xdr_u_int);

#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (this->is_open());

	if (len > 0)
	  xdr_vector(xdrs,
		     (char*) val,
		     len,
		     sizeof(unsigned int),
		     (xdrproc_t) xdr_u_int);

#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());

	for (unsigned int i=0; i<len; i++)
	  {
	    libmesh_assert(in.get());
            libmesh_assert (in->good());
	    *in >> val[i];
	  }

	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());

	if (line_break == libMesh::invalid_uint)
	  for (unsigned int i=0; i<len; i++)
	    {
	      libmesh_assert(out.get());
              libmesh_assert (out->good());
	      *out << val[i] << " ";
	    }
	else
	  {
	    unsigned int cnt=0;
	    while (cnt < len)
	      {
		for (unsigned int i=0; i<std::min(line_break,len); i++)
		  {
		    libmesh_assert(out.get());
                    libmesh_assert (out->good());
		    *out << val[cnt++] << " ";
		  }
		libmesh_assert(out.get());
                libmesh_assert (out->good());
		*out << '\n';
	      }
	  }

	return;
      }

    default:
      libmesh_error();
    }
}



template <>
void Xdr::data_stream (double *val, const unsigned int len, const unsigned int line_break)
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (this->is_open());

	if (len > 0)
	  xdr_vector(xdrs,
		     (char*) val,
		     len,
		     sizeof(double),
		     (xdrproc_t) xdr_double);

#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());

	for (unsigned int i=0; i<len; i++)
	  {
	    libmesh_assert(in.get());
            libmesh_assert (in->good());
	    *in >> val[i];
	  }

	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());

	if (line_break == libMesh::invalid_uint)
	  for (unsigned int i=0; i<len; i++)
	    {
	      libmesh_assert(out.get());
              libmesh_assert (out->good());
	      OFSRealscientific(*out,17,val[i]) << " ";
	    }
	else
	  {
	    unsigned int cnt=0;
	    while (cnt < len)
	      {
		for (unsigned int i=0; i<std::min(line_break,len); i++)
		  {
		    libmesh_assert(out.get());
                    libmesh_assert (out->good());
		    OFSRealscientific(*out,17,val[cnt++]) << " ";
		  }
		libmesh_assert(out.get());
                libmesh_assert (out->good());
		*out << '\n';
	      }
	  }

	return;
      }

    default:
      libmesh_error();
    }
}


template <>
void Xdr::data_stream (float *val, const unsigned int len, const unsigned int line_break)
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (this->is_open());

	if (len > 0)
	  xdr_vector(xdrs,
		     (char*) val,
		     len,
		     sizeof(float),
		     (xdrproc_t) xdr_float);

#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());

	for (unsigned int i=0; i<len; i++)
	  {
	    libmesh_assert(in.get());
            libmesh_assert (in->good());
	    *in >> val[i];
	  }

	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());

	if (line_break == libMesh::invalid_uint)
	  for (unsigned int i=0; i<len; i++)
	    {
	      libmesh_assert(out.get());
              libmesh_assert (out->good());
	      OFSRealscientific(*out,17,val[i]) << " ";
	    }
	else
	  {
	    unsigned int cnt=0;
	    while (cnt < len)
	      {
		for (unsigned int i=0; i<std::min(line_break,len); i++)
		  {
		    libmesh_assert(out.get());
                    libmesh_assert (out->good());
		    OFSRealscientific(*out,17,val[cnt++]) << " ";
		  }
		libmesh_assert(out.get());
                libmesh_assert (out->good());
		*out << '\n';
	      }
	  }

	return;
      }

    default:
      libmesh_error();
    }
}
template <>
void Xdr::data_stream (long double *val, const unsigned int len, const unsigned int line_break)
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (this->is_open());

	// FIXME[JWP]: How to implement this for long double?  Mac OS
	// X defines 'xdr_quadruple' but AFAICT, it does not exist for
	// Linux... for now, reading/writing XDR files with long
	// doubles drops back to double precision, but you can still
	// write long double ASCII files of course.
	// if (len > 0)
	//   xdr_vector(xdrs,
	// 	     (char*) val,
	// 	     len,
	// 	     sizeof(double),
	// 	     (xdrproc_t) xdr_quadruple);

	if (len > 0)
	  {
	    std::vector<double> io_buffer (len);

	    // Fill io_buffer if we are writing.
	    if (mode == ENCODE)
	      for (unsigned int i=0, cnt=0; i<len; i++)
		io_buffer[cnt++] = val[i];

	    xdr_vector(xdrs,
		       (char*) &io_buffer[0],
		       len,
		       sizeof(double),
		       (xdrproc_t) xdr_double);

	    // Fill val array if we are reading.
	    if (mode == DECODE)
	      for (unsigned int i=0, cnt=0; i<len; i++)
		{
		  val[i] = io_buffer[cnt++];
		}
	  }

#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());

	for (unsigned int i=0; i<len; i++)
	  {
	    libmesh_assert(in.get());
            libmesh_assert (in->good());
	    *in >> val[i];
	  }

	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());

	if (line_break == libMesh::invalid_uint)
	  for (unsigned int i=0; i<len; i++)
	    {
	      libmesh_assert(out.get());
              libmesh_assert (out->good());
	      OFSRealscientific(*out,17,val[i]) << " ";
	    }
	else
	  {
	    unsigned int cnt=0;
	    while (cnt < len)
	      {
		for (unsigned int i=0; i<std::min(line_break,len); i++)
		  {
		    libmesh_assert(out.get());
                    libmesh_assert (out->good());
		    OFSRealscientific(*out,17,val[cnt++]) << " ";
		  }
		libmesh_assert(out.get());
                libmesh_assert (out->good());
		*out << '\n';
	      }
	  }

	return;
      }

    default:
      libmesh_error();
    }
}


#ifdef LIBMESH_USE_COMPLEX_NUMBERS
template <>
void Xdr::data_stream (std::complex<double> *val, const unsigned int len, const unsigned int line_break)
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (this->is_open());


	if (len > 0)
	  {
	    std::vector<double> io_buffer (2*len);

	    // Fill io_buffer if we are writing.
	    if (mode == ENCODE)
	      for (unsigned int i=0, cnt=0; i<len; i++)
		{
		  io_buffer[cnt++] = val[i].real();
		  io_buffer[cnt++] = val[i].imag();
		}

	    xdr_vector(xdrs,
		       (char*) &io_buffer[0],
		       2*len,
		       sizeof(double),
		       (xdrproc_t) xdr_double);

	    // Fill val array if we are reading.
	    if (mode == DECODE)
	      for (unsigned int i=0, cnt=0; i<len; i++)
		{
		  double re = io_buffer[cnt++];
		  double im = io_buffer[cnt++];
                  val[i] = std::complex<double>(re,im);
		}
	  }
#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());

	for (unsigned int i=0; i<len; i++)
	  {
	    libmesh_assert(in.get());
            libmesh_assert (in->good());
            double re, im;
	    *in >> re >> im;
            val[i] = std::complex<double>(re,im);
	  }

	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());

	if (line_break == libMesh::invalid_uint)
	  for (unsigned int i=0; i<len; i++)
	    {
	      libmesh_assert(out.get());
              libmesh_assert (out->good());
	      OFSRealscientific(*out,17,val[i].real()) << " ";
	      OFSRealscientific(*out,17,val[i].imag()) << " ";
	    }
	else
	  {
	    unsigned int cnt=0;
	    while (cnt < len)
	      {
		for (unsigned int i=0; i<std::min(line_break,len); i++)
		  {
		    libmesh_assert(out.get());
                    libmesh_assert (out->good());
		    OFSRealscientific(*out,17,val[cnt].real()) << " ";
		    OFSRealscientific(*out,17,val[cnt].imag()) << " ";
		    cnt++;
		  }
		libmesh_assert(out.get());
                libmesh_assert (out->good());
		*out << '\n';
	      }
	  }

	return;
      }

    default:
      libmesh_error();
    }
}

template <>
void Xdr::data_stream (std::complex<long double> *val, const unsigned int len, const unsigned int line_break)
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
#ifdef LIBMESH_HAVE_XDR

	libmesh_assert (this->is_open());

	// FIXME[JWP]: How to implement this for long double?  Mac OS
	// X defines 'xdr_quadruple' but AFAICT, it does not exist for
	// Linux... for now, reading/writing XDR files with long
	// doubles drops back to double precision, but you can still
	// write long double ASCII files of course.

	if (len > 0)
	  {
	    std::vector<double> io_buffer (2*len);

	    // Fill io_buffer if we are writing.
	    if (mode == ENCODE)
	      for (unsigned int i=0, cnt=0; i<len; i++)
		{
		  io_buffer[cnt++] = val[i].real();
		  io_buffer[cnt++] = val[i].imag();
		}

	    xdr_vector(xdrs,
		       (char*) &io_buffer[0],
		       2*len,
		       sizeof(double),
		       (xdrproc_t) xdr_double);

	    // Fill val array if we are reading.
	    if (mode == DECODE)
	      for (unsigned int i=0, cnt=0; i<len; i++)
		{
		  double re = io_buffer[cnt++];
		  double im = io_buffer[cnt++];
		  val[i] = std::complex<long double>(re, im);
		}
	  }
#else

	libMesh::err << "ERROR: Functionality is not available." << std::endl
		      << "Make sure LIBMESH_HAVE_XDR is defined at build time"
		      << std::endl
		      << "The XDR interface is not available in this installation"
		      << std::endl;

	libmesh_error();

#endif
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());

	for (unsigned int i=0; i<len; i++)
	  {
	    libmesh_assert(in.get());
            libmesh_assert (in->good());
            long double re, im;
	    *in >> re >> im;
            val[i] = std::complex<long double>(re,im);
	  }

	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());

	if (line_break == libMesh::invalid_uint)
	  for (unsigned int i=0; i<len; i++)
	    {
	      libmesh_assert(out.get());
              libmesh_assert (out->good());
	      OFSRealscientific(*out,std::numeric_limits<long double>::digits10,val[i].real()) << " ";
	      OFSRealscientific(*out,std::numeric_limits<long double>::digits10,val[i].imag()) << " ";
	    }
	else
	  {
	    unsigned int cnt=0;
	    while (cnt < len)
	      {
		for (unsigned int i=0; i<std::min(line_break,len); i++)
		  {
		    libmesh_assert(out.get());
                    libmesh_assert (out->good());
		    OFSRealscientific(*out,std::numeric_limits<long double>::digits10,val[cnt].real()) << " ";
		    OFSRealscientific(*out,std::numeric_limits<long double>::digits10,val[cnt].imag()) << " ";
		    cnt++;
		  }
		libmesh_assert(out.get());
                libmesh_assert (out->good());
		*out << '\n';
	      }
	  }

	return;
      }

    default:
      libmesh_error();
    }
}
#endif // # LIBMESH_USE_COMPLEX_NUMBERS

void Xdr::comment (std::string &comment)
{
  switch (mode)
    {
    case ENCODE:
    case DECODE:
      {
	return;
      }

    case READ:
      {
	libmesh_assert(in.get());
        libmesh_assert (in->good());
	in->getline(comm, comm_len);
	return;
      }

    case WRITE:
      {
	libmesh_assert(out.get());
        libmesh_assert (out->good());
	*out << "\t " << comment << '\n';
	return;
      }

    default:
      libmesh_error();
    }
}


#undef xdr_REAL


//
template void Xdr::data<int>                              (int&,                             const char*);
template void Xdr::data<unsigned int>                     (unsigned int&,                    const char*);
template void Xdr::data<unsigned short int>               (unsigned short int&,              const char*);
template void Xdr::data<short int>                        (short int&,                       const char*);
template void Xdr::data<float>                            (float&,                           const char*);
template void Xdr::data<double>                           (double&,                          const char*);
template void Xdr::data<long double>                      (long double&,                     const char*);
template void Xdr::data<std::complex<float> >             (std::complex<float>&,             const char*);
template void Xdr::data<std::complex<double> >            (std::complex<double>&,            const char*);
template void Xdr::data<std::complex<long double> >       (std::complex<long double>&,       const char*);
template void Xdr::data<std::string>                      (std::string&,                     const char*);
template void Xdr::data<std::vector<int> >                (std::vector<int>&,                const char*);
template void Xdr::data<std::vector<unsigned int> >       (std::vector<unsigned int>&,       const char*);
template void Xdr::data<std::vector<short int> >          (std::vector<short int>&,          const char*);
template void Xdr::data<std::vector<unsigned short int> > (std::vector<unsigned short int>&, const char*);
template void Xdr::data<std::vector<float> >              (std::vector<float>&,              const char*);
template void Xdr::data<std::vector<double> >             (std::vector<double>&,             const char*);
template void Xdr::data<std::vector<long double> >        (std::vector<long double>&,        const char*);
template void Xdr::data<std::vector<std::complex<float> > >  (std::vector<std::complex<float> >&,  const char*);
template void Xdr::data<std::vector<std::complex<double> > > (std::vector<std::complex<double> >&, const char*);
template void Xdr::data<std::vector<std::complex<long double> > > (std::vector<std::complex<long double> >&, const char*);
template void Xdr::data_stream<int>          (int *val,          const unsigned int len, const unsigned int line_break);
template void Xdr::data_stream<unsigned int> (unsigned int *val, const unsigned int len, const unsigned int line_break);

} // namespace libMesh