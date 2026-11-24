#include "KernelRuntimeIdAllocator.hpp"
#include "DmicKernelOperationPlan.hpp"
#include "Ipc4KernelCodec.hpp"
#include <fstream>
#include <vector>
#include <cstdio>
using namespace sof::ipc4;
int main(int argc,char**argv) {
 if(argc!=2)return 1;
 std::ifstream in(argv[1],std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
 static kernel::FirmwareManifestView fw{}; if(kernel::parseFirmwareManifest(bytes.data(),bytes.size(),&fw)!=kernel::FirmwareManifestResult::Ok)return 2;
 dmic::kernel::RuntimeIdAllocator ids; if(ids.initialize(&fw,15)!=dmic::kernel::RuntimeIdAllocatorResult::Ok)return 3;
 dmic::kernel::RuntimeIds lease{}; if(ids.allocate(&lease)!=dmic::kernel::RuntimeIdAllocatorResult::Ok)return 4;
 kernel::HdaCaptureStreamEvidence e{7,0,1,true,true,true}; kernel::HdaDmicCapturePlan capture{};
 if(kernel::planHdaDmicCapture(e,&capture)!=kernel::HdaCapturePlanResult::Ok)return 5;
 dmic::kernel::OperationPlan plan; if(plan.initialize(&fw,lease,capture)!=dmic::kernel::OperationPlanResult::Ok)return 6;
 static kernel::Message msg{}; dmic::kernel::OperationKind kind{};
 for(unsigned i=0;i<19;i++) {
  if(plan.nextSetup(&msg,&kind)!=dmic::kernel::OperationPlanResult::Ok){printf("setup failed at %u\n",i);return 7;}
  if(i==11 && (msg.payloadBytes!=120 || msg.payload[40]!=1 || msg.payload[42]!=1 || msg.payload[60]!=0x80 || msg.payload[61]!=1))return 18;
  if(i==13 && (msg.payloadBytes!=84||msg.payload[68]!=0||msg.payload[69]!=1||msg.payload[72]!=0||msg.payload[73]!=6))return 8;
  if(i==17 && msg.payloadBytes!=808)return 9;
  if(i==18 && msg.payloadBytes!=128)return 10;
  if(plan.confirmCurrent(true)!=dmic::kernel::OperationPlanResult::Ok)return 11;
 }
 if(plan.nextSetup(&msg,&kind)!=dmic::kernel::OperationPlanResult::Complete)return 12;
 for(unsigned i=0;i<8;i++){if(plan.nextTeardown(&msg,&kind)!=dmic::kernel::OperationPlanResult::Ok)return 13;if(plan.confirmCurrent(true)!=dmic::kernel::OperationPlanResult::Ok)return 14;}
 if(plan.nextTeardown(&msg,&kind)!=dmic::kernel::OperationPlanResult::Complete)return 15;
 if(kernel::setTwoPipelineState(lease.frontendPipeline,lease.pipeline,kernel::kIpc4PipelineStateRunning,&msg)!=kernel::Result::Ok||msg.payloadBytes!=12||msg.payload[0]!=2||msg.payload[4]!=lease.frontendPipeline||msg.payload[8]!=lease.pipeline||msg.extension!=1)return 16;
 if(ids.release(lease,false)!=dmic::kernel::RuntimeIdAllocatorResult::TeardownUnconfirmed||!ids.hasActiveLease())return 19;
 if(ids.release(lease,true)!=dmic::kernel::RuntimeIdAllocatorResult::Ok)return 17;
 puts("firmware manifest, 9 ID leases, TDFB extended config, host gateway, 19 setup, 8 teardown and two-pipeline encoding passed"); return 0;
}
