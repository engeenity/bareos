/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2002-2011 Free Software Foundation Europe e.V.

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/**
 * Encode and decode Extended attributes for Win32 and
 * other non-Unix systems, or Unix systems with ACLs, ...
 *
 * Kern Sibbald, October MMII
 */

#include "bareos.h"
#include "find.h"
#include "ch.h"

static uid_t my_uid = 1;
static gid_t my_gid = 1;
static bool uid_set = false;

#if defined(HAVE_WIN32)
/* Forward referenced subroutines */
static bool set_win32_attributes(JCR *jcr, ATTR *attr, BFILE *ofd);
void unix_name_to_win32(POOLMEM **win32_name, char *name);
void win_error(JCR *jcr, const char *prefix, POOLMEM *ofile);
HANDLE bget_handle(BFILE *bfd);
#endif /* HAVE_WIN32 */

/* For old systems that don't have lchown() use chown() */
#ifndef HAVE_LCHOWN
#define lchown chown
#endif

/*=============================================================*/
/*                                                             */
/*             ***  A l l  S y s t e m s ***                   */
/*                                                             */
/*=============================================================*/

/**
 * Return the data stream that will be used
 */
int select_data_stream(FF_PKT *ff_pkt, bool compatible)
{
   int stream;

   /* This is a plugin special restore object */
   if (ff_pkt->type == FT_RESTORE_FIRST) {
      ff_pkt->flags = 0;
      return STREAM_FILE_DATA;
   }

   /*
    * Fix all incompatible options
    */

   /**
    * No sparse option for encrypted data
    */
   if (ff_pkt->flags & FO_ENCRYPT) {
      ff_pkt->flags &= ~FO_SPARSE;
   }

   /*
    * Note, no sparse option for win32_data
    */
   if (!is_portable_backup(&ff_pkt->bfd)) {
      stream = STREAM_WIN32_DATA;
      ff_pkt->flags &= ~FO_SPARSE;
   } else if (ff_pkt->flags & FO_SPARSE) {
      stream = STREAM_SPARSE_DATA;
   } else {
      stream = STREAM_FILE_DATA;
   }
   if (ff_pkt->flags & FO_OFFSETS) {
      stream = STREAM_SPARSE_DATA;
   }

   /*
    * Encryption is only supported for file data
    */
   if (stream != STREAM_FILE_DATA &&
       stream != STREAM_WIN32_DATA &&
       stream != STREAM_MACOS_FORK_DATA) {
      ff_pkt->flags &= ~FO_ENCRYPT;
   }

   /*
    * Compression is not supported for Mac fork data
    */
   if (stream == STREAM_MACOS_FORK_DATA) {
      ff_pkt->flags &= ~FO_COMPRESS;
   }

   /*
    * Handle compression and encryption options
    */
   if (ff_pkt->flags & FO_COMPRESS) {
      if (compatible && ff_pkt->Compress_algo == COMPRESS_GZIP) {
         switch (stream) {
         case STREAM_WIN32_DATA:
               stream = STREAM_WIN32_GZIP_DATA;
            break;
         case STREAM_SPARSE_DATA:
               stream = STREAM_SPARSE_GZIP_DATA;
            break;
         case STREAM_FILE_DATA:
               stream = STREAM_GZIP_DATA;
            break;
         default:
            /**
             * All stream types that do not support compression should clear out
             * FO_COMPRESS above, and this code block should be unreachable.
             */
            ASSERT(!(ff_pkt->flags & FO_COMPRESS));
            return STREAM_NONE;
         }
      } else {
         switch (stream) {
         case STREAM_WIN32_DATA:
            stream = STREAM_WIN32_COMPRESSED_DATA;
            break;
         case STREAM_SPARSE_DATA:
            stream = STREAM_SPARSE_COMPRESSED_DATA;
            break;
         case STREAM_FILE_DATA:
            stream = STREAM_COMPRESSED_DATA;
            break;
         default:
            /*
             * All stream types that do not support compression should clear out
             * FO_COMPRESS above, and this code block should be unreachable.
             */
            ASSERT(!(ff_pkt->flags & FO_COMPRESS));
            return STREAM_NONE;
         }
      }
   }

#ifdef HAVE_CRYPTO
   if (ff_pkt->flags & FO_ENCRYPT) {
      switch (stream) {
      case STREAM_WIN32_DATA:
         stream = STREAM_ENCRYPTED_WIN32_DATA;
         break;
      case STREAM_WIN32_GZIP_DATA:
         stream = STREAM_ENCRYPTED_WIN32_GZIP_DATA;
         break;
      case STREAM_WIN32_COMPRESSED_DATA:
         stream = STREAM_ENCRYPTED_WIN32_COMPRESSED_DATA;
         break;
      case STREAM_FILE_DATA:
         stream = STREAM_ENCRYPTED_FILE_DATA;
         break;
      case STREAM_GZIP_DATA:
         stream = STREAM_ENCRYPTED_FILE_GZIP_DATA;
         break;
      case STREAM_COMPRESSED_DATA:
         stream = STREAM_ENCRYPTED_FILE_COMPRESSED_DATA;
         break;
      default:
         /*
          * All stream types that do not support encryption should clear out
          * FO_ENCRYPT above, and this code block should be unreachable.
          */
         ASSERT(!(ff_pkt->flags & FO_ENCRYPT));
         return STREAM_NONE;
      }
   }
#endif

   return stream;
}

/**
 * Set file modes, permissions and times
 *
 *  fname is the original filename
 *  ofile is the output filename (may be in a different directory)
 *
 * Returns:  true  on success
 *           false on failure
 */
bool set_attributes(JCR *jcr, ATTR *attr, BFILE *ofd)
{
   struct utimbuf ut;
   mode_t old_mask;
   bool ok = true;
   boffset_t fsize;

   if (uid_set) {
      my_uid = getuid();
      my_gid = getgid();
      uid_set = true;
   }

#if defined(HAVE_WIN32)
   if (attr->stream == STREAM_UNIX_ATTRIBUTES_EX &&
       set_win32_attributes(jcr, attr, ofd)) {
       if (is_bopen(ofd)) {
           bclose(ofd);
       }
       pm_strcpy(attr->ofname, "*none*");
       return true;
   }
   if (attr->data_stream == STREAM_WIN32_DATA ||
       attr->data_stream == STREAM_WIN32_GZIP_DATA ||
       attr->data_stream == STREAM_WIN32_COMPRESSED_DATA) {
      if (is_bopen(ofd)) {
         bclose(ofd);
      }
      pm_strcpy(attr->ofname, "*none*");
      return true;
   }

   /**
    * If Windows stuff failed, e.g. attempt to restore Unix file
    *  to Windows, simply fall through and we will do it the
    *  universal way.
    */
#endif

   old_mask = umask(0);
   if (is_bopen(ofd)) {
      char ec1[50], ec2[50];
      fsize = blseek(ofd, 0, SEEK_END);
      bclose(ofd);                    /* first close file */
      if (attr->type == FT_REG && fsize > 0 && attr->statp.st_size > 0 &&
                        fsize != (boffset_t)attr->statp.st_size) {
         Jmsg3(jcr, M_ERROR, 0, _("File size of restored file %s not correct. Original %s, restored %s.\n"),
            attr->ofname, edit_uint64(attr->statp.st_size, ec1),
            edit_uint64(fsize, ec2));
      }
   }

   /**
    * We do not restore sockets, so skip trying to restore their
    *   attributes.
    */
   if (attr->type == FT_SPEC && S_ISSOCK(attr->statp.st_mode)) {
      goto bail_out;
   }

   ut.actime = attr->statp.st_atime;
   ut.modtime = attr->statp.st_mtime;

   /* ***FIXME**** optimize -- don't do if already correct */
   /**
    * For link, change owner of link using lchown, but don't
    *   try to do a chmod as that will update the file behind it.
    */
   if (attr->type == FT_LNK) {
      /** Change owner of link, not of real file */
      if (lchown(attr->ofname, attr->statp.st_uid, attr->statp.st_gid) < 0 && my_uid == 0) {
         berrno be;
         Jmsg2(jcr, M_ERROR, 0, _("Unable to set file owner %s: ERR=%s\n"),
            attr->ofname, be.bstrerror());
         ok = false;
      }
   } else {
      if (chown(attr->ofname, attr->statp.st_uid, attr->statp.st_gid) < 0 && my_uid == 0) {
         berrno be;
         Jmsg2(jcr, M_ERROR, 0, _("Unable to set file owner %s: ERR=%s\n"),
            attr->ofname, be.bstrerror());
         ok = false;
      }
      if (chmod(attr->ofname, attr->statp.st_mode) < 0 && my_uid == 0) {
         berrno be;
         Jmsg2(jcr, M_ERROR, 0, _("Unable to set file modes %s: ERR=%s\n"),
            attr->ofname, be.bstrerror());
         ok = false;
      }

      /**
       * Reset file times.
       */
      if (utime(attr->ofname, &ut) < 0 && my_uid == 0) {
         berrno be;
         Jmsg2(jcr, M_ERROR, 0, _("Unable to set file times %s: ERR=%s\n"),
            attr->ofname, be.bstrerror());
         ok = false;
      }
#ifdef HAVE_CHFLAGS
      /**
       * FreeBSD user flags
       *
       * Note, this should really be done before the utime() above,
       *  but if the immutable bit is set, it will make the utimes()
       *  fail.
       */
      if (chflags(attr->ofname, attr->statp.st_flags) < 0 && my_uid == 0) {
         berrno be;
         Jmsg2(jcr, M_ERROR, 0, _("Unable to set file flags %s: ERR=%s\n"),
            attr->ofname, be.bstrerror());
         ok = false;
      }
#endif
   }

bail_out:
   pm_strcpy(attr->ofname, "*none*");
   umask(old_mask);
   return ok;
}

#if !defined(HAVE_WIN32)
/*=============================================================*/
/*                                                             */
/*                 * * *  U n i x * * * *                      */
/*                                                             */
/*=============================================================*/

/**
 * It is possible to piggyback additional data e.g. ACLs on
 *   the encode_stat() data by returning the extended attributes
 *   here.  They must be "self-contained" (i.e. you keep track
 *   of your own length), and they must be in ASCII string
 *   format. Using this feature is not recommended.
 * The code below shows how to return nothing.  See the Win32
 *   code below for returning something in the attributes.
 */
int encode_attribsEx(JCR *jcr, char *attribsEx, FF_PKT *ff_pkt)
{
#ifdef HAVE_DARWIN_OS
   /**
    * We save the Mac resource fork length so that on a
    * restore, we can be sure we put back the whole resource.
    */
   char *p;

   *attribsEx = 0;                 /* no extended attributes (yet) */
   if (jcr->cmd_plugin || ff_pkt->type == FT_DELETED) {
      return STREAM_UNIX_ATTRIBUTES;
   }
   p = attribsEx;
   if (ff_pkt->flags & FO_HFSPLUS) {
      p += to_base64((uint64_t)(ff_pkt->hfsinfo.rsrclength), p);
   }
   *p = 0;
#else
   *attribsEx = 0;                    /* no extended attributes */
#endif
   return STREAM_UNIX_ATTRIBUTES;
}
#else
/*=============================================================*/
/*                                                             */
/*                 * * *  W i n 3 2 * * * *                    */
/*                                                             */
/*=============================================================*/
int encode_attribsEx(JCR *jcr, char *attribsEx, FF_PKT *ff_pkt)
{
   char *p = attribsEx;
   WIN32_FILE_ATTRIBUTE_DATA atts;
   ULARGE_INTEGER li;

   attribsEx[0] = 0;                  /* no extended attributes */

   if (jcr->cmd_plugin || ff_pkt->type == FT_DELETED) {
      return STREAM_UNIX_ATTRIBUTES;
   }

   unix_name_to_win32(&ff_pkt->sys_fname, ff_pkt->fname);

   /** try unicode version */
   if (p_GetFileAttributesExW)  {
      POOLMEM* pwszBuf = get_pool_memory(PM_FNAME);
      make_win32_path_UTF8_2_wchar(&pwszBuf, ff_pkt->fname);

      BOOL b=p_GetFileAttributesExW((LPCWSTR)pwszBuf, GetFileExInfoStandard,
                                    (LPVOID)&atts);
      free_pool_memory(pwszBuf);

      if (!b) {
         win_error(jcr, "GetFileAttributesExW:", ff_pkt->sys_fname);
         return STREAM_UNIX_ATTRIBUTES;
      }
   }
   else {
      if (!p_GetFileAttributesExA)
         return STREAM_UNIX_ATTRIBUTES;

      if (!p_GetFileAttributesExA(ff_pkt->sys_fname, GetFileExInfoStandard,
                              (LPVOID)&atts)) {
         win_error(jcr, "GetFileAttributesExA:", ff_pkt->sys_fname);
         return STREAM_UNIX_ATTRIBUTES;
      }
   }

   p += to_base64((uint64_t)atts.dwFileAttributes, p);
   *p++ = ' ';                        /* separate fields with a space */
   li.LowPart = atts.ftCreationTime.dwLowDateTime;
   li.HighPart = atts.ftCreationTime.dwHighDateTime;
   p += to_base64((uint64_t)li.QuadPart, p);
   *p++ = ' ';
   li.LowPart = atts.ftLastAccessTime.dwLowDateTime;
   li.HighPart = atts.ftLastAccessTime.dwHighDateTime;
   p += to_base64((uint64_t)li.QuadPart, p);
   *p++ = ' ';
   li.LowPart = atts.ftLastWriteTime.dwLowDateTime;
   li.HighPart = atts.ftLastWriteTime.dwHighDateTime;
   p += to_base64((uint64_t)li.QuadPart, p);
   *p++ = ' ';
   p += to_base64((uint64_t)atts.nFileSizeHigh, p);
   *p++ = ' ';
   p += to_base64((uint64_t)atts.nFileSizeLow, p);
   *p = 0;
   return STREAM_UNIX_ATTRIBUTES_EX;
}

/* Define attributes that are legal to set with SetFileAttributes() */
#define SET_ATTRS ( \
         FILE_ATTRIBUTE_ARCHIVE| \
         FILE_ATTRIBUTE_HIDDEN| \
         FILE_ATTRIBUTE_NORMAL| \
         FILE_ATTRIBUTE_NOT_CONTENT_INDEXED| \
         FILE_ATTRIBUTE_OFFLINE| \
         FILE_ATTRIBUTE_READONLY| \
         FILE_ATTRIBUTE_SYSTEM| \
         FILE_ATTRIBUTE_TEMPORARY)

/* Do casting according to unknown type to keep compiler happy */
#ifdef HAVE_TYPEOF
  #define plug(st, val) st = (typeof st)val
#else
  /* Use templates to do the casting */
  template <class T> void plug(T &st, uint64_t val)
  { st = static_cast<T>(val); }
#endif

/**
 * Set Extended File Attributes for Win32
 *
 *  fname is the original filename
 *  ofile is the output filename (may be in a different directory)
 *
 * Returns:  true  on success
 *           false on failure
 */
static bool set_win32_attributes(JCR *jcr, ATTR *attr, BFILE *ofd)
{
   char *p = attr->attrEx;
   int64_t val;
   WIN32_FILE_ATTRIBUTE_DATA atts;
   ULARGE_INTEGER li;
   POOLMEM *win32_ofile;

   /** if we have neither Win ansi nor wchar API, get out */
   if (!(p_SetFileAttributesW || p_SetFileAttributesA)) {
      return false;
   }

   if (!p || !*p) {                   /* we should have attributes */
      Dmsg2(100, "Attributes missing. of=%s ofd=%d\n", attr->ofname, ofd->fid);
      if (is_bopen(ofd)) {
         bclose(ofd);
      }
      return false;
   } else {
      Dmsg2(100, "Attribs %s = %s\n", attr->ofname, attr->attrEx);
   }

   p += from_base64(&val, p);
   plug(atts.dwFileAttributes, val);
   p++;                               /* skip space */
   p += from_base64(&val, p);
   li.QuadPart = val;
   atts.ftCreationTime.dwLowDateTime = li.LowPart;
   atts.ftCreationTime.dwHighDateTime = li.HighPart;
   p++;                               /* skip space */
   p += from_base64(&val, p);
   li.QuadPart = val;
   atts.ftLastAccessTime.dwLowDateTime = li.LowPart;
   atts.ftLastAccessTime.dwHighDateTime = li.HighPart;
   p++;                               /* skip space */
   p += from_base64(&val, p);
   li.QuadPart = val;
   atts.ftLastWriteTime.dwLowDateTime = li.LowPart;
   atts.ftLastWriteTime.dwHighDateTime = li.HighPart;
   p++;
   p += from_base64(&val, p);
   plug(atts.nFileSizeHigh, val);
   p++;
   p += from_base64(&val, p);
   plug(atts.nFileSizeLow, val);

   /** Convert to Windows path format */
   win32_ofile = get_pool_memory(PM_FNAME);
   unix_name_to_win32(&win32_ofile, attr->ofname);

   /** At this point, we have reconstructed the WIN32_FILE_ATTRIBUTE_DATA pkt */

   if (!is_bopen(ofd)) {
      Dmsg1(100, "File not open: %s\n", attr->ofname);
      bopen(ofd, attr->ofname, O_WRONLY|O_BINARY, 0);   /* attempt to open the file */
   }

   if (is_bopen(ofd)) {
      Dmsg1(100, "SetFileTime %s\n", attr->ofname);
      if (!SetFileTime(bget_handle(ofd),
                         &atts.ftCreationTime,
                         &atts.ftLastAccessTime,
                         &atts.ftLastWriteTime)) {
         win_error(jcr, "SetFileTime:", win32_ofile);
      }
      bclose(ofd);
   }

   Dmsg1(100, "SetFileAtts %s\n", attr->ofname);
   if (!(atts.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      if (p_SetFileAttributesW) {
         POOLMEM* pwszBuf = get_pool_memory(PM_FNAME);
         make_win32_path_UTF8_2_wchar(&pwszBuf, attr->ofname);

         BOOL b=p_SetFileAttributesW((LPCWSTR)pwszBuf, atts.dwFileAttributes & SET_ATTRS);
         free_pool_memory(pwszBuf);

         if (!b)
            win_error(jcr, "SetFileAttributesW:", win32_ofile);
      }
      else {
         if (!p_SetFileAttributesA(win32_ofile, atts.dwFileAttributes & SET_ATTRS)) {
            win_error(jcr, "SetFileAttributesA:", win32_ofile);
         }
      }
   }
   free_pool_memory(win32_ofile);
   return true;
}

void win_error(JCR *jcr, const char *prefix, POOLMEM *win32_ofile)
{
   DWORD lerror = GetLastError();
   LPTSTR msg;
   FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|
                 FORMAT_MESSAGE_FROM_SYSTEM,
                 NULL,
                 lerror,
                 0,
                 (LPTSTR)&msg,
                 0,
                 NULL);
   Dmsg3(100, "Error in %s on file %s: ERR=%s\n", prefix, win32_ofile, msg);
   strip_trailing_junk(msg);
   Jmsg3(jcr, M_ERROR, 0, _("Error in %s file %s: ERR=%s\n"), prefix, win32_ofile, msg);
   LocalFree(msg);
}

void win_error(JCR *jcr, const char *prefix, DWORD lerror)
{
   LPTSTR msg;
   FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|
                 FORMAT_MESSAGE_FROM_SYSTEM,
                 NULL,
                 lerror,
                 0,
                 (LPTSTR)&msg,
                 0,
                 NULL);
   strip_trailing_junk(msg);
   if (jcr) {
      Jmsg2(jcr, M_ERROR, 0, _("Error in %s: ERR=%s\n"), prefix, msg);
   } else {
      MessageBox(NULL, msg, prefix, MB_OK);
   }
   LocalFree(msg);
}
#endif  /* HAVE_WIN32 */
