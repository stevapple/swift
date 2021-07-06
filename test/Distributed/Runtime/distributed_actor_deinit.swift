// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-distributed -parse-as-library) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// REQUIRES: radar78290608

import _Distributed

@available(SwiftStdlib 5.5, *)
actor A {}

@available(SwiftStdlib 5.5, *)
distributed actor DA {
}

@available(SwiftStdlib 5.5, *)
distributed actor DA_userDefined {
  deinit {}
}

@available(SwiftStdlib 5.5, *)
distributed actor DA_userDefined2 {
  deinit {
    print("Deinitializing \(self.actorAddress)")
    return
  }
}

@available(SwiftStdlib 5.5, *)
distributed actor DA_state {
  var name = "Hello"
  var age = 42

  deinit {
    print("Deinitializing \(self.actorAddress)")
    return
  }
}

// ==== Fake Transport ---------------------------------------------------------

@available(SwiftStdlib 5.5, *)
struct FakeTransport: ActorTransport {
  func resolve<Act>(address: ActorAddress, as actorType: Act.Type)
    throws -> ActorResolved<Act> where Act: DistributedActor {
    print("resolve type:\(actorType), address:\(address)")
    return .makeProxy
  }

  func assignAddress<Act>(
    _ actorType: Act.Type
  ) -> ActorAddress where Act : DistributedActor {
    let address = ActorAddress(parse: "xxx")
    print("assign type:\(actorType), address:\(address)")
    return address
  }

  public func actorReady<Act>(
    _ actor: Act
  ) where Act: DistributedActor {
    print("ready actor:\(actor), address:\(actor.actorAddress)")
  }

  public func resignAddress(
    _ address: ActorAddress
  ) {
    print("resign address:\(address)")
  }
}

// ==== Execute ----------------------------------------------------------------

@available(SwiftStdlib 5.5, *)
func test() {
  let transport = FakeTransport()
  let address = ActorAddress(parse: "xxx")

  // no lifecycle things make sense for a normal actor, double check we didn't emit them
  print("before A")
  _ = A()
  print("after A")
  // CHECK: before A
  // CHECK: after A

  _ = DA(transport: transport)
  // CHECK: assign type:DA, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA, address:[[ADDRESS]]
  // CHECK: resign address:[[ADDRESS]]

  _ = DA_userDefined(transport: transport)
  // CHECK: assign type:DA_userDefined, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_userDefined, address:[[ADDRESS]]
  // CHECK: resign address:[[ADDRESS]]

  // resign must happen as the _last thing_ after user-deinit completed
  _ = DA_userDefined2(transport: transport)
  // CHECK: assign type:DA_userDefined2, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_userDefined2, address:[[ADDRESS]]
  // CHECK: Deinitializing [[ADDRESS]]
  // CHECK-NEXT: resign address:[[ADDRESS]]

  // resign must happen as the _last thing_ after user-deinit completed
  _ = DA_state(transport: transport)
  // CHECK: assign type:DA_state, address:[[ADDRESS:.*]]
  // CHECK: ready actor:main.DA_state, address:[[ADDRESS]]
  // CHECK: Deinitializing [[ADDRESS]]
  // CHECK-NEXT: resign address:[[ADDRESS]]

  // a remote actor should not resign it's address, it was never "assigned" it
  print("before")
  _ = try! DA_userDefined2(resolve: address, using: transport)
  print("done")
  // CHECK: before
  // CHECK-NEXT: Deinitializing
  // CHECK-NEXT: done
}

@available(SwiftStdlib 5.5, *)
@main struct Main {
  static func main() async {
    test()
  }
}
