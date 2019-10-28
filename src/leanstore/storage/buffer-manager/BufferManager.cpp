#include "BufferManager.hpp"
#include "BufferFrame.hpp"
#include "leanstore/random-generator/RandomGenerator.hpp"
#include "leanstore/storage/buffer-manager/SmartPointer.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
// -------------------------------------------------------------------------------------
#include <fcntl.h>
#include <unistd.h>
// -------------------------------------------------------------------------------------
DEFINE_uint32(dram_pages, 1024, "");
DEFINE_uint32(ssd_pages, 2048, "");
DEFINE_uint32(page_size, 16 * 1024, "");
DEFINE_string(ssd_path, "leanstore", "");
DEFINE_bool(ssd_truncate, true, "");
// -------------------------------------------------------------------------------------
DEFINE_uint32(cooling_threshold, 10, "Start cooling pages when 100-x% are free");
DEFINE_uint32(background_write_sleep, 10, "us");
DEFINE_uint32(write_buffer_size, 10, "");
// -------------------------------------------------------------------------------------
namespace leanstore {
BufferManager::BufferManager()
{
   // -------------------------------------------------------------------------------------
   // Init DRAM pool
   buffer_frame_size = FLAGS_page_size + sizeof(BufferFrame);
   const u64 dram_total_size = buffer_frame_size * FLAGS_dram_pages;
   dram = reinterpret_cast<u8 *>(mmap(NULL, dram_total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
   madvise(dram, dram_total_size, MADV_HUGEPAGE);
   memset(dram, 0, dram_total_size);
   // -------------------------------------------------------------------------------------
   /// Init SSD pool
   const u32 ssd_total_size = FLAGS_ssd_pages * FLAGS_page_size;
   int flags = O_RDWR | O_DIRECT | O_CREAT;
   if ( FLAGS_ssd_truncate ) {
      flags |= O_TRUNC;
   }
   ssd_fd = open(FLAGS_ssd_path.c_str(), flags, 0666);
   ftruncate(ssd_fd, ssd_total_size);
   check(ssd_fd > -1);
   if ( fcntl(ssd_fd, F_GETFL) == -1 ) {
      throw Generic_Exception("Can not initialize SSD storage: " + FLAGS_ssd_path);
   }
   // Init AIO stack
   memset(&ssd_aio_context, 0, sizeof(ssd_aio_context));
   if ( io_setup(10, &ssd_aio_context) != 0 ) {
      throw Generic_Exception("io_setup failed");
   }
   for ( auto i = 0; i < FLAGS_write_buffer_size; i++ ) {
      write_buffer_free_slots.push_front(i);
   }
   // -------------------------------------------------------------------------------------
   for ( u64 pid = 0; pid < FLAGS_dram_pages; pid++ ) {
      dram_free_bfs.push(new(dram + (pid * buffer_frame_size)) BufferFrame(pid));
   }
   for ( u64 pid = 0; pid < FLAGS_ssd_pages; pid++ ) {
      cooling_io_ht.emplace(std::piecewise_construct, std::forward_as_tuple(pid), std::forward_as_tuple());
      ssd_free_pages.push(pid);
   }
   // -------------------------------------------------------------------------------------
   std::thread page_provider_thread([&]() {
      while ( true ) {
         usleep(FLAGS_background_write_sleep);
      }
   });
   threads_handle.push_back(page_provider_thread.native_handle());
   page_provider_thread.detach();
   // -------------------------------------------------------------------------------------
   //
   std::thread checkpoint_thread([&]() {
      while ( true ) {
         try {
            auto rand_buffer_i = RandomGenerator::getRand<u64>(0, FLAGS_dram_pages);
            BufferFrame &rand_buffer = *reinterpret_cast<BufferFrame *>(dram + (buffer_frame_size * rand_buffer_i));
            SharedLock lock(rand_buffer.header.lock);
            if ( rand_buffer.header.lastWrittenLSN != rand_buffer.page.LSN ) {
               lock.recheck();
               writePageAsync(rand_buffer);
            }
            usleep(FLAGS_background_write_sleep);
         } catch ( RestartException e ) {

         }
      }
   });
   threads_handle.push_back(checkpoint_thread.native_handle());
   checkpoint_thread.detach();
   // -------------------------------------------------------------------------------------
   std::thread aio_pooling_thread([&]() {
      const u32 event_max_nr = 10;
      struct io_event events[event_max_nr];
      struct timespec timeout;
      u64 polled_events_nr = 0;
      while ( true ) {
         try {
            timeout.tv_sec = 0;
            timeout.tv_nsec = 500000000;
            if ( polled_events_nr = io_getevents(ssd_aio_context, 0, event_max_nr, events, &timeout)) {
               ssd_aio_mutex.lock();
               for ( auto i = 0; i < polled_events_nr; i++ ) {
                  BufferFrame *bf = std::get<1>(ssd_aio_ht[events[i].obj->key]);
                  auto write_buffer_slot = std::get<0> (ssd_aio_ht[events[i].obj->key]);
                  auto page_buffer = write_buffer.get() + (FLAGS_page_size * write_buffer_slot);
                  while(true) {
                     try {
                        SharedLock lock(bf->header.lock);
                        ExclusiveLock x_lock(lock);
                        bf->header.isWB = false;
                        bf->header.lastWrittenLSN = reinterpret_cast<BufferFrame::Page *>(page_buffer)->LSN.load();
                        memset(page_buffer, 0, FLAGS_page_size);
                        write_buffer_free_slots.push_front(write_buffer_slot);
                        break;
                     } catch(RestartException e) {

                     }
                  }
               }
               ssd_aio_mutex.unlock();
               break;
            }
            sleep(1);
         } catch ( RestartException e ) {

         }
      }
   });
   threads_handle.push_back(aio_pooling_thread.native_handle());
   aio_pooling_thread.detach();
   // -------------------------------------------------------------------------------------
}
// -------------------------------------------------------------------------------------
// Buffer Frames Management
// -------------------------------------------------------------------------------------
BufferFrame *BufferManager::getLoadedBF(PID pid)
{
   return reinterpret_cast<BufferFrame *>(dram + (pid * buffer_frame_size));
}
// -------------------------------------------------------------------------------------
void BufferManager::checkCoolingThreshold()
{

}
// -------------------------------------------------------------------------------------
u64 BufferManager::accquirePage()
{
   std::lock_guard lock(reservoir_mutex);
   auto free_bf = ssd_free_pages.front();
   ssd_free_pages.pop();
   return free_bf;
}
// -------------------------------------------------------------------------------------
BufferFrame &BufferManager::accquireBufferFrame()
{
   std::lock_guard lock(reservoir_mutex);
   auto free_bf = dram_free_bfs.front();
   dram_free_bfs.pop();
   dram_used_bfs.push(free_bf);
   return *free_bf;
}
// -------------------------------------------------------------------------------------
BufferFrame &BufferManager::fixPage(BufferFrame &swip_holder, Swip &swip)
{
   if ( swip.isSwizzled()) {
      return swip.getBufferFrame();
   }
   global_mutex.lock();
   if ( swip.isSwizzled()) { // maybe another thread has already fixed it
      return swip.getBufferFrame();
   }
   CIOFrame &cio_frame = cooling_io_ht.find(swip.asInteger())->second;
   if ( cio_frame.state == CIOFrame::State::NOT_LOADED ) {
      cio_frame.readers_counter++;
      cio_frame.mutex.lock();
      global_mutex.unlock();
      BufferFrame &bf = accquireBufferFrame();
      readPageSync(swip.asInteger(), bf.page);
      cio_frame.mutex.unlock();
      throw RestartException();
      // TODO: do we really need to clean up ?
   }
   if ( cio_frame.state == CIOFrame::State::READING ) {
      cio_frame.readers_counter++;
      global_mutex.unlock();
      cio_frame.mutex.lock();
      cio_frame.mutex.unlock();
      throw RestartException();
   }
   if ( cio_frame.state == CIOFrame::State::COOLING ) {
      while ( true ) {
         try {
            SharedLock lock(swip_holder.header.lock);
            ExclusiveLock x_lock(lock);
            BufferFrame *bf = *cio_frame.fifo_itr;
            cooling_fifo_queue.erase(cio_frame.fifo_itr);
            swip.swizzle(bf);
            global_mutex.unlock();
            return *bf;
         } catch ( RestartException e ) {
         }
      }
   }
   UNREACHABLE();
}
// -------------------------------------------------------------------------------------
// SSD management
// -------------------------------------------------------------------------------------
void BufferManager::readPageSync(u64 pid, u8 *destination)
{
   s64 read_bytes = pread(ssd_fd, destination, FLAGS_page_size, pid * FLAGS_page_size);
   check(read_bytes == FLAGS_page_size);
}
// -------------------------------------------------------------------------------------
void BufferManager::writePageAsync(BufferFrame &bf)
{
   write_buffer = make_unique<u8[]>(FLAGS_page_size * FLAGS_write_buffer_size);
   while ( true ) {
      try {
         ssd_aio_mutex.lock();
         SharedLock bf_lock(bf.header.lock);
         ExclusiveLock bf_x_lock(bf_lock);
         auto src = reinterpret_cast<u8 *>(&bf.page);
         assert(u64(src) % 512 == 0);
         if ( write_buffer_free_slots.size() == 0 ) {
            throw RestartException();
         }
         auto buffer_slot = write_buffer_free_slots.front();
         write_buffer_free_slots.pop_front();
         ssd_aio_mutex.unlock();
         auto write_buffer_copy = write_buffer.get() + (FLAGS_page_size * buffer_slot);
         std::memcpy(write_buffer_copy, src, FLAGS_page_size);
         {
            struct iocb iocb;
            struct iocb *iocbs[1];
            io_prep_pwrite(&iocb, ssd_fd, (void *) write_buffer_copy, FLAGS_page_size, bf.header.pid * FLAGS_page_size);
            iocb.data = (void *) write_buffer_copy;
            iocbs[0] = &iocb;
            if ( io_submit(ssd_aio_context, 1, iocbs) != 1 ) {
               throw Generic_Exception("io_submit failed");
            }
            ssd_aio_ht.insert({iocb.key, {buffer_slot, &bf}});
         }
      } catch ( RestartException e ) {

      }
   }
}
// -------------------------------------------------------------------------------------
void BufferManager::flush()
{
   fdatasync(ssd_fd);
}
// -------------------------------------------------------------------------------------
unique_ptr<BufferManager> BMC::global_bf(nullptr);
void BMC::start()
{
   global_bf = make_unique<BufferManager>();
}
// -------------------------------------------------------------------------------------
BufferManager::~BufferManager()
{
   for(const auto &handle: threads_handle){
      pthread_cancel(handle);
   }
   u32 dram_page_size = FLAGS_page_size + sizeof(BufferFrame);
   const u32 dram_total_size = dram_page_size * FLAGS_dram_pages;
   munmap(dram, dram_total_size);
   close(ssd_fd);
   ssd_fd = -1;
   io_destroy(ssd_aio_context);
   // -------------------------------------------------------------------------------------
   // TODO: save states in YAML
}
// -------------------------------------------------------------------------------------
}
// -------------------------------------------------------------------------------------