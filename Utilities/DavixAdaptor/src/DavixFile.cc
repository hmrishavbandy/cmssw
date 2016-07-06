#include "Utilities/DavixAdaptor/interface/DavixFile.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/Utilities/interface/Exception.h"
#include <cassert>
#include <davix.hpp>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

using namespace Davix;

static Context *davix_context_s = NULL;

DavixFile::DavixFile(void) {}

DavixFile::DavixFile(const char *name, int flags /* = IOFlags::OpenRead */, int perms /* = 066 */) {
  open(name, flags, perms);
}

DavixFile::DavixFile(const std::string &name, int flags /* = IOFlags::OpenRead */,
                     int perms /* = 066 */) {
  open(name.c_str(), flags, perms);
}

DavixFile::~DavixFile(void) {
  configureDavixLogLevel();
  close();
  return;
}

Context *DavixFile::getDavixInstance() {
  if (davix_context_s == NULL) {
    davix_context_s = new Context();
  }
  return davix_context_s;
}

void DavixFile::close(void) {
  if (davixPosix != NULL && m_fd != NULL) {
    DavixError *err;
    davixPosix->close(m_fd, &err);
    delete davixPosix;
  }
  return;
}

void DavixFile::abort(void) {
  if (davixPosix != NULL && m_fd != NULL) {
    DavixError *err;
    davixPosix->close(m_fd, &err);
    delete davixPosix;
  }
  return;
}

void DavixFile::configureDavixLogLevel() {
  long logLevel = 0;
  char *logptr;
  char *davixDebug = getenv("Davix_Debug");
  if (davixDebug != NULL) {
    logLevel = strtol(davixDebug, &logptr, 0);
    if (errno) {
      edm::LogWarning("DavixFile") << "Got error while converting "
                                   << "Davix_Debug env variable to integer. "
                                      "Will use default log level 0";
      logLevel = 0;
    }
    if (logptr == davixDebug) {
      edm::LogWarning("DavixFile") << "Failed to convert to integer "
                                   << "Davix_Debug env variable; Will use default log level 0";
      logLevel = 0;
    } else if (*logptr != '\0') {
      edm::LogWarning("DavixFile") << "Failed to parse extra junk "
                                   << "from Davix_Debug env variable. Will use default log level 0";
      logLevel = 0;
    }
  }
  switch (logLevel) {
  case 0:
    davix_set_log_level(0);
    break;
  case 1:
    davix_set_log_level(DAVIX_LOG_WARNING);
    break;
  case 2:
    davix_set_log_level(DAVIX_LOG_VERBOSE);
    break;
  case 3:
    davix_set_log_level(DAVIX_LOG_DEBUG);
    break;
  default:
    davix_set_log_level(DAVIX_LOG_ALL);
    break;
  }
}

static int X509Authentication(void *userdata, const SessionInfo &info, X509Credential *cert,
                              DavixError **davixErr) {
  std::string ucert, ukey;
  char default_proxy[64];
  snprintf(default_proxy, sizeof(default_proxy), "/tmp/x509up_u%d", geteuid());
  // X509_USER_PROXY
  if (getenv("X509_USER_PROXY")) {
    edm::LogInfo("DavixFile") << "X509_USER_PROXY found in environment."
                              << " Will use it for authentication";
    ucert = ukey = getenv("X509_USER_PROXY");
  }
  // Default proxy location
  else if (access(default_proxy, R_OK) == 0) {
    edm::LogInfo("DavixFile") << "Found proxy in default location " << default_proxy
                              << " Will use it for authentication";
    ucert = ukey = default_proxy;
  }
  // X509_USER_CERT
  else if (getenv("X509_USER_CERT")) {
    ucert = getenv("X509_USER_CERT");
  }
  // X509_USER_KEY only if X509_USER_CERT was found
  if (!ucert.empty() && getenv("X509_USER_KEY")) {
    edm::LogInfo("DavixFile") << "X509_USER_{CERT|KEY} found in environment"
                              << " Will use it for authentication";
    ukey = getenv("X509_USER_KEY");
  }
  // Check if vars are set...
  if (ucert.empty() || ukey.empty()) {
    edm::LogWarning("DavixFile") << "Was not able to find proxy in $X509_USER_PROXY, "
                                 << "X509_USER_{CERT|KEY} or default proxy creation location. "
                                 << "Will try without authentication";
    return -1;
  }
  return cert->loadFromFilePEM(ukey, ucert, "", davixErr);
}

void DavixFile::create(const char *name, bool exclusive /* = false */, int perms /* = 066 */) {
  open(name, (IOFlags::OpenCreate | IOFlags::OpenWrite | IOFlags::OpenTruncate |
              (exclusive ? IOFlags::OpenExclusive : 0)),
       perms);
}

void DavixFile::create(const std::string &name, bool exclusive /* = false */,
                       int perms /* = 066 */) {
  open(name.c_str(), (IOFlags::OpenCreate | IOFlags::OpenWrite | IOFlags::OpenTruncate |
                      (exclusive ? IOFlags::OpenExclusive : 0)),
       perms);
}

void DavixFile::open(const std::string &name, int flags /* = IOFlags::OpenRead */,
                     int perms /* = 066 */) {
  open(name.c_str(), flags, perms);
}

void DavixFile::open(const char *name, int flags /* = IOFlags::OpenRead */, int perms /* = 066 */) {
  // Actual open
  if ((name == 0) || (*name == 0)) {
    edm::Exception ex(edm::errors::FileOpenError);
    ex << "Cannot open a file without name";
    ex.addContext("Calling DavixFile::open()");
    throw ex;
  }

  if ((flags & IOFlags::OpenRead) == 0) {
    edm::Exception ex(edm::errors::FileOpenError);
    ex << "Must open file '" << name << "' at least for read";
    ex.addContext("Calling DavixFile::open()");
    throw ex;
  }

  configureDavixLogLevel();
  // Translate our flags to system flags
  int openflags = 0;

  if (flags & IOFlags::OpenRead)
    openflags |= O_RDONLY;

  DavixError *davixErr;
  davixReqParams = new RequestParams();
  // Set up X509 authentication
  davixReqParams->setClientCertCallbackX509(&X509Authentication, NULL);
  // Set also CERT_DIR if it is set in envinroment, otherwise use default
  const char *cert_dir = NULL;
  if ((cert_dir = getenv("X509_CERT_DIR")) == NULL)
    cert_dir = "/etc/grid-security/certificates";
  davixReqParams->addCertificateAuthorityPath(cert_dir);

  davixPosix = new DavPosix(getDavixInstance());
  m_fd = davixPosix->open(davixReqParams, name, openflags, &davixErr);

  // Check Davix Error
  if (davixErr) {
    edm::Exception ex(edm::errors::FileOpenError);
    ex << "Davix::open(name='" << m_name << "') failed with "
       << "error '" << davixErr->getErrMsg().c_str() << " and error code " << davixErr->getStatus();
    ex.addContext("Calling DavixFile::open()");
    throw ex;
  }
  if (m_fd == NULL) {
    edm::Exception ex(edm::errors::FileOpenError);
    ex << "Davix::open(name='" << m_name << "') failed as fd is NULL";
    ex.addContext("Calling DavixFile::open()");
    throw ex;
  }
  m_name = name;
}

IOSize DavixFile::readv(IOBuffer *into, IOSize buffers) {
  assert(!buffers || into);

  // Davix does not support 0 buffers;
  if (buffers == 0)
    return 0;

  DavixError *davixErr;

  DavIOVecInput input_vector[buffers];
  DavIOVecOuput output_vector[buffers];
  IOSize total = 0; // Total requested bytes
  for (IOSize i = 0; i < buffers; ++i) {
    input_vector[i].diov_size = into[i].size();
    input_vector[i].diov_buffer = (char *)into[i].data();
    total += into[i].size();
  }

  ssize_t s = davixPosix->preadVec(m_fd, input_vector, output_vector, buffers, &davixErr);
  if (davixErr) {
    edm::Exception ex(edm::errors::FileReadError);
    ex << "Davix::readv(name='" << m_name << "', buffers=" << (buffers) << ") failed with error "
       << davixErr->getErrMsg().c_str() << " and error code " << davixErr->getStatus()
       << " and call returned " << s << " bytes";
    ex.addContext("Calling DavixFile::readv()");
    throw ex;
  }
  if (s < 0) {
    edm::Exception ex(edm::errors::FileReadError);
    ex << "Davix::readv(name='" << m_name << "') failed and call returned " << s;
    ex.addContext("Calling DavixFile::readv()");
    throw ex;
  }
  return total;
}

IOSize DavixFile::readv(IOPosBuffer *into, IOSize buffers) {
  assert(!buffers || into);

  // Davix does not support 0 buffers;
  if (buffers == 0)
    return 0;

  DavixError *davixErr;

  DavIOVecInput input_vector[buffers];
  DavIOVecOuput output_vector[buffers];
  IOSize total = 0;
  for (IOSize i = 0; i < buffers; ++i) {
    input_vector[i].diov_offset = into[i].offset();
    input_vector[i].diov_size = into[i].size();
    input_vector[i].diov_buffer = (char *)into[i].data();
    total += into[i].size();
  }
  ssize_t s = davixPosix->preadVec(m_fd, input_vector, output_vector, buffers, &davixErr);
  if (davixErr) {
    edm::Exception ex(edm::errors::FileReadError);
    ex << "Davix::readv(name='" << m_name << "', n=" << buffers << ") failed with error "
       << davixErr->getErrMsg().c_str() << " and error code " << davixErr->getStatus()
       << " and call returned " << s << " bytes";
    ex.addContext("Calling DavixFile::readv()");
    throw ex;
  }
  if (s < 0) {
    edm::Exception ex(edm::errors::FileReadError);
    ex << "Davix::readv(name='" << m_name << "', n=" << buffers << ") failed and call returned "
       << s;
    ex.addContext("Calling DavixFile::readv()");
    throw ex;
  } else if (s == 0)
    return 0; // end of file

  return total;
}

IOSize DavixFile::read(void *into, IOSize n) {
  DavixError *davixErr;
  davixPosix->fadvise(m_fd, 0, n, AdviseRandom);
  IOSize done = 0;
  while (done < n) {
    ssize_t s = davixPosix->read(m_fd, (char *)into + done, n - done, &davixErr);
    if (davixErr) {
      edm::Exception ex(edm::errors::FileReadError);
      ex << "Davix::read(name='" << m_name << "', n=" << (n - done) << ") failed with error "
         << davixErr->getErrMsg().c_str() << " and error code " << davixErr->getStatus()
         << " and call returned " << s << " bytes";
      ex.addContext("Calling DavixFile::read()");
      throw ex;
    }
    if (s < 0) {
      edm::Exception ex(edm::errors::FileReadError);
      ex << "Davix::read(name='" << m_name << "', n=" << (n - done) << ") failed and call returned "
         << s;
      ex.addContext("Calling DavixFile::read()");
      throw ex;
    } else if (s == 0)
      // end of file
      break;
    done += s;
  }
  return done;
}

IOSize DavixFile::write(const void *from, IOSize n) {
  cms::Exception ex("FileWriteError");
  ex << "DavixFile::write(name='" << m_name << "') not implemented";
  throw ex;
}

IOOffset DavixFile::position(IOOffset offset, Relative whence /* = SET */) {
  DavixError *davixErr;
  if (whence != CURRENT && whence != SET && whence != END) {
    cms::Exception ex("FilePositionError");
    ex << "DavixFile::position() called with incorrect 'whence' parameter";
    throw ex;
  }
  IOOffset result;
  size_t mywhence = (whence == SET ? SEEK_SET : whence == CURRENT ? SEEK_CUR : SEEK_END);

  if ((result = davixPosix->lseek(m_fd, offset, mywhence, &davixErr)) == -1) {
    cms::Exception ex("FilePositionError");
    ex << "Davix::lseek(name='" << m_name << "', offset=" << offset << ", whence=" << mywhence
       << ") failed with error " << davixErr->getErrMsg().c_str() << " and "
       << "error code " << davixErr->getStatus() << " and "
       << "call returned " << result;
    ex.addContext("Calling DavixFile::position()");
    throw ex;
  }

  return result;
}

void DavixFile::resize(IOOffset /* size */) {
  cms::Exception ex("FileResizeError");
  ex << "DavixFile::resize(name='" << m_name << "') not implemented";
  throw ex;
}
