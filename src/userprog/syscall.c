#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"
 
 
static int sys_halt (void);
static int sys_exit (int status);
static int sys_exec (const char *ufile);
static int sys_wait (tid_t);
static int sys_create (const char *ufile, unsigned initial_size);
static int sys_remove (const char *ufile);
static int sys_open (const char *ufile);
static int sys_filesize (int handle);
static int sys_read (int handle, void *udst_, unsigned size);
static int sys_write (int handle, void *usrc_, unsigned size);
static int sys_seek (int handle, unsigned position);
static int sys_tell (int handle);
static int sys_close (int handle);
static int sys_mmap (int handle, void *addr);
static int sys_munmap (int mapping);
static int sys_chdir (const char *udir);
static int sys_mkdir (const char *udir);
static int sys_readdir (int handle, char *name);
static int sys_isdir (int handle);
static int sys_inumber (int handle);
 
static void syscall_handler (struct intr_frame *);
static void copy_in (void *, const void *, size_t);
 
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}
 
/* System call handler. */
static void
syscall_handler (struct intr_frame *f) 
{
  typedef int syscall_function (int, int, int);

  /* A system call. */
  struct syscall 
    {
      size_t arg_cnt;           /* Number of arguments. */
      syscall_function *func;   /* Implementation. */
    };

  /* Table of system calls. */
  static const struct syscall syscall_table[] =
    {
      {0, (syscall_function *) sys_halt},
      {1, (syscall_function *) sys_exit},
      {1, (syscall_function *) sys_exec},
      {1, (syscall_function *) sys_wait},
      {2, (syscall_function *) sys_create},
      {1, (syscall_function *) sys_remove},
      {1, (syscall_function *) sys_open},
      {1, (syscall_function *) sys_filesize},
      {3, (syscall_function *) sys_read},
      {3, (syscall_function *) sys_write},
      {2, (syscall_function *) sys_seek},
      {1, (syscall_function *) sys_tell},
      {1, (syscall_function *) sys_close},
      {2, (syscall_function *) sys_mmap},
      {1, (syscall_function *) sys_munmap},
      {1, (syscall_function *) sys_chdir},
      {1, (syscall_function *) sys_mkdir},
      {2, (syscall_function *) sys_readdir},
      {1, (syscall_function *) sys_isdir},
      {1, (syscall_function *) sys_inumber},
    };

  const struct syscall *sc;
  unsigned call_nr;
  int args[3];

  /* Get the system call. */
  copy_in (&call_nr, f->esp, sizeof call_nr);
  if (call_nr >= sizeof syscall_table / sizeof *syscall_table)
    thread_exit ();
  sc = syscall_table + call_nr;

  /* Get the system call arguments. */
  ASSERT (sc->arg_cnt <= sizeof args / sizeof *args);
  memset (args, 0, sizeof args);
  copy_in (args, (uint32_t *) f->esp + 1, sizeof *args * sc->arg_cnt);

  /* Execute the system call,
     and set the return value. */
  f->eax = sc->func (args[0], args[1], args[2]);
}
 
/* Copies SIZE bytes from user address USRC to kernel address
   DST.
   Call thread_exit() if any of the user accesses are invalid. */
static void
copy_in (void *dst_, const void *usrc_, size_t size) 
{
  uint8_t *dst = dst_;
  const uint8_t *usrc = usrc_;

  while (size > 0) 
    {
      size_t chunk_size = PGSIZE - pg_ofs (usrc);
      if (chunk_size > size)
        chunk_size = size;
      
      if (!page_lock (usrc, false))
        thread_exit ();
      memcpy (dst, usrc, chunk_size);
      page_unlock (usrc);

      dst += chunk_size;
      usrc += chunk_size;
      size -= chunk_size;
    }
}
 
/* Copies SIZE bytes from kernel address SRC to user address
   UDST.
   Call thread_exit() if any of the user accesses are invalid. */
static void
copy_out (void *udst_, const void *src_, size_t size) 
{
  uint8_t *udst = udst_;
  const uint8_t *src = src_;

  while (size > 0) 
    {
      size_t chunk_size = PGSIZE - pg_ofs (udst);
      if (chunk_size > size)
        chunk_size = size;
      
      if (!page_lock (udst, false))
        thread_exit ();
      memcpy (udst, src, chunk_size);
      page_unlock (udst);

      udst += chunk_size;
      src += chunk_size;
      size -= chunk_size;
    }
}
 
/* Creates a copy of user string US in kernel memory
   and returns it as a page that must be freed with
   palloc_free_page().
   Truncates the string at PGSIZE bytes in size.
   Call thread_exit() if any of the user accesses are invalid. */
static char *
copy_in_string (const char *us) 
{
  char *ks;
  char *upage;
  size_t length;
 
  ks = palloc_get_page (0);
  if (ks == NULL) 
    thread_exit ();

  length = 0;
  for (;;) 
    {
      upage = pg_round_down (us);
      if (!page_lock (upage, false))
        goto lock_error;

      for (; us < upage + PGSIZE; us++) 
        {
          ks[length++] = *us;
          if (*us == '\0') 
            {
              page_unlock (upage);
              return ks; 
            }
          else if (length >= PGSIZE) 
            goto too_long_error;
        }

      page_unlock (upage);
    }

 too_long_error:
  page_unlock (upage);
 lock_error:
  palloc_free_page (ks);
  thread_exit ();
}
 
/* Halt system call. */
static int
sys_halt (void)
{
  shutdown_power_off ();
}
 
/* Exit system call. */
static int
sys_exit (int exit_code) 
{
  thread_current ()->exit_code = exit_code;
  thread_exit ();
  NOT_REACHED ();
}
 
/* Exec system call. */
static int
sys_exec (const char *ufile) 
{
  tid_t tid;
  char *kfile = copy_in_string (ufile);
 
  tid = process_execute (kfile);
 
  palloc_free_page (kfile);
 
  return tid;
}
 
/* Wait system call. */
static int
sys_wait (tid_t child) 
{
  return process_wait (child);
}
 
/* Create system call. */
static int
sys_create (const char *ufile, unsigned initial_size) 
{
  char *kfile = copy_in_string (ufile);
  bool ok = filesys_create (kfile, initial_size, FILE_INODE);
  palloc_free_page (kfile);
 
  return ok;
}
 
/* Remove system call. */
static int
sys_remove (const char *ufile) 
{
  char *kfile = copy_in_string (ufile);
  bool ok = filesys_remove (kfile);
  palloc_free_page (kfile);
 
  return ok;
}

/* A file descriptor, for binding a file handle to a file. */
struct file_descriptor
  {
    struct list_elem elem;      /* List element. */
    struct file *file;          /* File. */
    struct dir *dir;            /* Directory. */
    int handle;                 /* File handle. */
  };
 
/* Open system call. */
static int
sys_open (const char *ufile) 
{
  char *kfile = copy_in_string (ufile);
  struct file_descriptor *fd;
  int handle = -1;
 
  fd = calloc (1, sizeof *fd);
  if (fd != NULL)
    {
      struct inode *inode = filesys_open (kfile);
      if (inode != NULL)
        {
          if (inode_get_type (inode) == FILE_INODE)
            fd->file = file_open (inode);
          else
            fd->dir = dir_open (inode);
          if (fd->file != NULL || fd->dir != NULL)
            {
              struct thread *cur = thread_current ();
              handle = fd->handle = cur->next_handle++;
              list_push_front (&cur->fds, &fd->elem);
            }
          else 
            {
              free (fd);
              inode_close (inode);
            }
        }
    }
  
  palloc_free_page (kfile);
  return handle;
}
 
/* Returns the file descriptor associated with the given handle.
   Terminates the process if HANDLE is not associated with an
   open file. */
static struct file_descriptor *
lookup_fd (int handle) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
   
  for (e = list_begin (&cur->fds); e != list_end (&cur->fds);
       e = list_next (e))
    {
      struct file_descriptor *fd;
      fd = list_entry (e, struct file_descriptor, elem);
      if (fd->handle == handle)
        return fd;
    }
 
  thread_exit ();
}
 
/* Returns the file descriptor associated with the given handle.
   Terminates the process if HANDLE is not associated with an
   open ordinary file. */
static struct file_descriptor *
lookup_file_fd (int handle) 
{
  struct file_descriptor *fd = lookup_fd (handle);
  if (fd->file == NULL)
    thread_exit ();
  return fd;
}
 
/* Returns the file descriptor associated with the given handle.
   Terminates the process if HANDLE is not associated with an
   open directory. */
static struct file_descriptor *
lookup_dir_fd (int handle) 
{
  struct file_descriptor *fd = lookup_fd (handle);
  if (fd->dir == NULL)
    thread_exit ();
  return fd;
}

/* Filesize system call. */
static int
sys_filesize (int handle) 
{
  struct file_descriptor *fd = lookup_file_fd (handle);
  int size;
 
  size = file_length (fd->file);
 
  return size;
}
 
/* Read system call. */
static int
sys_read (int handle, void *udst_, unsigned size) 
{
  uint8_t *udst = udst_;
  struct file_descriptor *fd;
  int bytes_read = 0;

  /* Look up file descriptor. */
  if (handle != STDIN_FILENO)
    fd = lookup_file_fd (handle);

  while (size > 0) 
    {
      /* How much to read into this page? */
      size_t page_left = PGSIZE - pg_ofs (udst);
      size_t read_amt = size < page_left ? size : page_left;
      off_t retval;

      /* Check that touching this page is okay. */
      if (!page_lock (udst, true)) 
        thread_exit ();

      /* Read from file into page. */
      if (handle != STDIN_FILENO) 
        {
          retval = file_read (fd->file, udst, read_amt);
          if (retval < 0)
            {
              if (bytes_read == 0)
                bytes_read = -1; 
              break;
            }
          bytes_read += retval; 
        }
      else 
        {
          size_t i;
          
          for (i = 0; i < read_amt; i++) 
            udst[i] = input_getc ();
          bytes_read = read_amt;
        }

      /* Release page. */
      page_unlock (udst);

      /* If it was a short read we're done. */
      if (retval != (off_t) read_amt)
        break;

      /* Advance. */
      udst += retval;
      size -= retval;
    }
   
  return bytes_read;
}
 
/* Write system call. */
static int
sys_write (int handle, void *usrc_, unsigned size) 
{
  uint8_t *usrc = usrc_;
  struct file_descriptor *fd = NULL;
  int bytes_written = 0;

  /* Lookup up file descriptor. */
  if (handle != STDOUT_FILENO)
    fd = lookup_file_fd (handle);

  while (size > 0) 
    {
      /* How much bytes to write to this page? */
      size_t page_left = PGSIZE - pg_ofs (usrc);
      size_t write_amt = size < page_left ? size : page_left;
      off_t retval;

      /* Check that we can touch this user page. */
      if (!page_lock (usrc, false)) 
        thread_exit ();

      /* Do the write. */
      if (handle == STDOUT_FILENO)
        {
          putbuf ((char *) usrc, write_amt);
          retval = write_amt;
        }
      else
        retval = file_write (fd->file, usrc, write_amt);

      /* Release user page. */
      page_unlock (usrc);

      /* Handle return value. */
      if (retval < 0) 
        {
          if (bytes_written == 0)
            bytes_written = -1;
          break;
        }
      bytes_written += retval;

      /* If it was a short write we're done. */
      if (retval != (off_t) write_amt)
        break;

      /* Advance. */
      usrc += retval;
      size -= retval;
    }
 
  return bytes_written;
}
 
/* Seek system call. */
static int
sys_seek (int handle, unsigned position) 
{
  if ((off_t) position >= 0)
    file_seek (lookup_file_fd (handle)->file, position);
  return 0;
}
 
/* Tell system call. */
static int
sys_tell (int handle) 
{
  return file_tell (lookup_file_fd (handle)->file);
}
 
/* Close system call. */
static int
sys_close (int handle) 
{
  struct file_descriptor *fd = lookup_fd (handle);
  file_close (fd->file);
  dir_close (fd->dir);
  list_remove (&fd->elem);
  free (fd);
  return 0;
}

/* Binds a mapping id to a region of memory and a file. */
struct mapping
  {
    struct list_elem elem;      /* List element. */
    int handle;                 /* Mapping id. */
    struct file *file;          /* File. */
    uint8_t *base;              /* Start of memory mapping. */
    size_t page_cnt;            /* Number of pages mapped. */
  };

/* Returns the file descriptor associated with the given handle.
   Terminates the process if HANDLE is not associated with a
   memory mapping. */
static struct mapping *
lookup_mapping (int handle) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
   
  for (e = list_begin (&cur->mappings); e != list_end (&cur->mappings);
       e = list_next (e))
    {
      struct mapping *m = list_entry (e, struct mapping, elem);
      if (m->handle == handle)
        return m;
    }
 
  thread_exit ();
}

/* Remove mapping M from the virtual address space,
   writing back any pages that have changed. */
static void
unmap (struct mapping *m) 
{
  list_remove (&m->elem);
  while (m->page_cnt-- > 0) 
    {
      page_deallocate (m->base);
      m->base += PGSIZE;
    }
  file_close (m->file);
  free (m);
}
 
/* Mmap system call. */
static int
sys_mmap (int handle, void *addr)
{
  struct file_descriptor *fd = lookup_file_fd (handle);
  struct mapping *m = malloc (sizeof *m);
  size_t offset;
  off_t length;

  if (m == NULL || addr == NULL || pg_ofs (addr) != 0)
    return -1;

  m->handle = thread_current ()->next_handle++;
  m->file = file_reopen (fd->file);
  if (m->file == NULL) 
    {
      free (m);
      return -1;
    }
  m->base = addr;
  m->page_cnt = 0;
  list_push_front (&thread_current ()->mappings, &m->elem);

  offset = 0;
  length = file_length (m->file);
  while (length > 0)
    {
      struct page *p = page_allocate ((uint8_t *) addr + offset, false);
      if (p == NULL)
        {
          unmap (m);
          return -1;
        }
      p->private = false;
      p->file = m->file;
      p->file_offset = offset;
      p->file_bytes = length >= PGSIZE ? PGSIZE : length;
      offset += p->file_bytes;
      length -= p->file_bytes;
      m->page_cnt++;
    }
  
  return m->handle;
}

/* Munmap system call. */
static int
sys_munmap (int mapping) 
{
  unmap (lookup_mapping (mapping));
  return 0;
}

/* Chdir system call. */
static int
sys_chdir (const char *udir) 
{
  bool ok = false;

  // ADD CODE HERE
  char *kdir = copy_in_string(udir);
  ok = filesys_chdir(kdir);
  palloc_free_page(kdir);

  return ok;
}

/* Mkdir system call. */
static int
sys_mkdir (const char *udir)
{
  char *kdir = copy_in_string (udir);
  bool ok = filesys_create (kdir, 0, DIR_INODE);
  palloc_free_page (kdir);
 
  return ok;
}

/* Readdir system call. */
static int
sys_readdir (int handle, char *uname)
{
  struct file_descriptor *fd = lookup_dir_fd (handle);
  char name[NAME_MAX + 1];
  bool ok = dir_readdir (fd->dir, name);
  if (ok)
    copy_out (uname, name, strlen (name) + 1);
  return ok;
}

/* Isdir system call. */
static int
sys_isdir (int handle)
{
  struct file_descriptor *fd = lookup_fd (handle);
  return fd->dir != NULL;
}

/* Inumber system call. */
static int
sys_inumber (int handle)
{
  // ADD AND MODIFY CODE HERE - call dir_get_inode() for directories
  if(sys_isdir(handle))
  {
    struct file_descriptor *dir_descriptor = lookup_dir_fd(handle);
    struct inode *inode = dir_get_inode(dir_descriptor);
    return inode_get_inumber(inode);
  }

  struct file_descriptor *fd = lookup_fd (handle);
  struct inode *inode = file_get_inode (fd->file);
  return inode_get_inumber (inode);
}
 
/* On thread exit, close all open files and unmap all mappings. */
void
syscall_exit (void) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e, *next;
   
  for (e = list_begin (&cur->fds); e != list_end (&cur->fds); e = next)
    {
      struct file_descriptor *fd = list_entry (e, struct file_descriptor, elem);
      next = list_next (e);
      file_close (fd->file);
      dir_close (fd->dir);
      free (fd);
    }
   
  for (e = list_begin (&cur->mappings); e != list_end (&cur->mappings);
       e = next)
    {
      struct mapping *m = list_entry (e, struct mapping, elem);
      next = list_next (e);
      unmap (m);
    }

  dir_close (cur->wd);
}
