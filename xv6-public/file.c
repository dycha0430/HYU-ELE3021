//
// File descriptors
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "buf.h"
#include "stat.h"

uint write_buf_no[NBUF];
int index = 0;
/*
#define min(a, b) ((a) < (b) ? (a) : (b))
extern uint bmap(struct inode *ip, uint bn);
*/
extern void commit();
struct bcache {
  struct spinlock lock;
  struct buf buf[NBUF];
  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.
  struct buf head;
};

struct logheader {
  int n;
  int block[LOGSIZE];
};

extern struct bcache bcache;
struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};

extern struct log log;
struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE)
    pipeclose(ff.pipe, ff.writable);
  else if(ff.type == FD_INODE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
int
filestat(struct file *f, struct stat *st)
{
  if(f->type == FD_INODE){
    ilock(f->ip);
    stati(f->ip, st);
    iunlock(f->ip);
    return 0;
  }
  return -1;
}

// Read from file f.
int
fileread(struct file *f, char *addr, int n)
{
  int r;

  if(f->readable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return piperead(f->pipe, addr, n);
  if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
    return r;
  }
  panic("fileread");
}

//PAGEBREAK!
// Write to file f.
int
filewrite(struct file *f, char *addr, int n)
{
  int r;
  if(f->writable == 0)
    return -1;
  
  if(f->type == FD_PIPE)
    return pipewrite(f->pipe, addr, n);
  
  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    //int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;

    while (i < n) {
        int n1 = n - i;
        if (n1 > max) n1 = max;

        begin_op();
        ilock(f->ip);
        if ((r = writei2(f->ip, addr + i, f->off, n1)) > 0) f->off += r;
        iunlock(f->ip);
        end_op();

        if (r < 0) break;
        if (r != n1) panic("short filewrite");
        i += r;
    /*
    uint tot, m, off;
    struct buf *bp;
    
    while (i < n) {
      // TODO
      int ret = 0;
      int n1 = n - i;
      if (n1 > max) n1 = max;

      begin_op();
      ilock(f->ip);

      //////////////////////////////////////
      //if ((r = writei(f->ip, addr + i, f->off, n1)) > 0) f->off += r;
      //////////////////////////////////////

      
      off = f->off;
      if (f->ip->type == T_DEV) {
        if(f->ip->major < 0 || f->ip->major >= NDEV || !devsw[f->ip->major].write)
          ret = -1;
        ret = devsw[f->ip->major].write(f->ip, addr, n1);
      }
      

      cprintf("off : %d, n1 : %d, f->ip->size : %d\n", off, n1, f->ip->size);
      if (off > f->ip->size || off + n1 < off) ret = -1;
      if (off + n1 > MAXFILE*BSIZE) ret = -1;
      
      if (ret == 0) {
        for (tot = 0; tot < n1; tot += m, off += m, addr += m) {
          uint blockno = bmap(f->ip, off/BSIZE);
          write_buf_no[index++] = blockno;
          bp = bread(f->ip->dev, blockno);
        
          m = min(n1 - tot, BSIZE - off%BSIZE);
          memmove(bp->data + off%BSIZE, addr, m);
          brelse(bp);
        }
      
        if (n1 > 0 && off > f->ip->size) {
          f->ip->size = off;
          iupdate(f->ip);
        }

        ret = n1;
      }
      
      if (ret > 0) f->off += n1;
      
      iunlock(f->ip);
      end_op();
 
      if (ret < 0) break;
      if (ret != n1) panic("short filewrite");
      i += ret;
      */
    }
    
    return i == n ? n : -1;
  }
  
  
  panic("filewrite");
}


int sync(void) {
  struct buf *b;
  begin_op();
  
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    //cprintf("Syncing.. %s %d %d\n", &b->data[0], log.start, log.start + log.lh.n);
    for (int i = 0; i < index; i++) {
        if (b->blockno == write_buf_no[i]) {
            log_write(b);
            if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1) commit();
            break;
        }
    }
  }
  index = 0;
  
  
  end_op();

  return 0;
}

int get_log_num(void) {
    return log.lh.n;
}

int pwrite(struct file* f, void* addr, int n, int off) {
  int r;

  if(f->writable == 0)
    return -1;
  
  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;
    uint tmp_off = off;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, addr + i, tmp_off, n1)) > 0)
        tmp_off += r;
      iunlock(f->ip);
      end_op();

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    return i == n ? n : -1;
  }
  panic("filewrite");
}   

int pread(struct file* f, void* addr, int n, int off) {
  int r;

  if(f->readable == 0)
    return -1;
  
  if(f->type == FD_INODE){
    ilock(f->ip);
    r = readi(f->ip, addr, off, n);
    iunlock(f->ip);
    return r;
  }
  panic("fileread");
 
}
