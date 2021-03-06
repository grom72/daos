//
// (C) Copyright 2018-2020 Intel Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
// The Government's rights to use, modify, reproduce, release, perform, display,
// or disclose this software are subject to the terms of the Apache License as
// provided in Contract No. 8F-30005.
// Any reproduction of computer software, computer software documentation, or
// portions thereof marked with this legend must also reproduce the markings.
//

package server

import (
	"net"
	"os/exec"
	"strconv"
	"strings"

	"github.com/golang/protobuf/proto"
	"github.com/pkg/errors"
	"golang.org/x/net/context"

	"github.com/daos-stack/daos/src/control/common"
	mgmtpb "github.com/daos-stack/daos/src/control/common/proto/mgmt"
	"github.com/daos-stack/daos/src/control/drpc"
	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/system"
)

// CheckReplica verifies if this server is supposed to host an MS replica,
// only performing the check and printing the result for now.
func CheckReplica(lis net.Listener, accessPoints []string, srv *exec.Cmd) (msReplicaCheck string, err error) {
	isReplica, bootstrap, err := checkMgmtSvcReplica(lis.Addr().(*net.TCPAddr), accessPoints)
	if err != nil {
		_ = srv.Process.Kill()
		return
	}

	if isReplica {
		msReplicaCheck = " as access point"
		if bootstrap {
			msReplicaCheck += " (bootstrap)"
		}
	}

	return
}

// getInterfaceAddrs enables TestCheckMgmtSvcReplica to replace the real
// interface query with a sample data set.
var getInterfaceAddrs = func() ([]net.Addr, error) {
	return net.InterfaceAddrs()
}

// checkMgmtSvcReplica determines if this server is supposed to host an MS
// replica, based on this server's management address and the system access
// points. If bootstrap is true, in which case isReplica must be true, this
// replica shall bootstrap the MS.
func checkMgmtSvcReplica(self *net.TCPAddr, accessPoints []string) (isReplica, bootstrap bool, err error) {
	replicas, err := resolveAccessPoints(accessPoints)
	if err != nil {
		return false, false, err
	}

	selves, err := getListenIPs(self)
	if err != nil {
		return false, false, err
	}

	// Check each replica against this server's listen IPs.
	for i := range replicas {
		if replicas[i].Port != self.Port {
			continue
		}
		for _, ip := range selves {
			if ip.Equal(replicas[i].IP) {
				// The first replica in the access point list
				// shall bootstrap the MS.
				if i == 0 {
					return true, true, nil
				}
				return true, false, nil
			}
		}
	}

	return false, false, nil
}

// resolveAccessPoints resolves the strings in accessPoints into addresses in
// addrs. If a port isn't specified, assume the default port.
func resolveAccessPoints(accessPoints []string) (addrs []*net.TCPAddr, err error) {
	defaultPort := NewConfiguration().ControlPort
	for _, ap := range accessPoints {
		if !common.HasPort(ap) {
			ap = net.JoinHostPort(ap, strconv.Itoa(defaultPort))
		}
		t, err := net.ResolveTCPAddr("tcp", ap)
		if err != nil {
			return nil, err
		}
		addrs = append(addrs, t)
	}
	return addrs, nil
}

// getListenIPs takes the address this server listens on and returns a list of
// the corresponding IPs.
func getListenIPs(listenAddr *net.TCPAddr) (listenIPs []net.IP, err error) {
	if listenAddr.IP.IsUnspecified() {
		// Find the IPs of all IP interfaces.
		addrs, err := getInterfaceAddrs()
		if err != nil {
			return nil, err
		}
		for _, a := range addrs {
			// Ignore non-IP interfaces.
			in, ok := a.(*net.IPNet)
			if ok {
				listenIPs = append(listenIPs, in.IP)
			}
		}
	} else {
		listenIPs = append(listenIPs, listenAddr.IP)
	}
	return listenIPs, nil
}

// mgmtSvc implements (the Go portion of) Management Service, satisfying
// mgmtpb.MgmtSvcServer.
type mgmtSvc struct {
	log              logging.Logger
	harness          *IOServerHarness
	membership       *system.Membership // if MS leader, system membership list
	clientNetworkCfg *ClientNetworkCfg
}

func newMgmtSvc(h *IOServerHarness, m *system.Membership, c *ClientNetworkCfg) *mgmtSvc {
	return &mgmtSvc{
		log:              h.log,
		harness:          h,
		membership:       m,
		clientNetworkCfg: c,
	}
}

func (svc *mgmtSvc) GetAttachInfo(ctx context.Context, req *mgmtpb.GetAttachInfoReq) (*mgmtpb.GetAttachInfoResp, error) {
	if svc.clientNetworkCfg == nil {
		return nil, errors.New("clientNetworkCfg is missing")
	}

	mi, err := svc.harness.GetMSLeaderInstance()
	if err != nil {
		return nil, err
	}

	dresp, err := mi.CallDrpc(drpc.ModuleMgmt, drpc.MethodGetAttachInfo, req)
	if err != nil {
		return nil, err
	}

	resp := &mgmtpb.GetAttachInfoResp{}
	if err = proto.Unmarshal(dresp.Body, resp); err != nil {
		return nil, errors.Wrap(err, "unmarshal GetAttachInfo response")
	}

	resp.Provider = svc.clientNetworkCfg.Provider
	resp.CrtCtxShareAddr = svc.clientNetworkCfg.CrtCtxShareAddr
	resp.CrtTimeout = svc.clientNetworkCfg.CrtTimeout

	return resp, nil
}

// checkIsMSReplica provides a hint as to who is service leader if instance is
// not a Management Service replica.
func checkIsMSReplica(mi *IOServerInstance) error {
	msg := "instance is not an access point"
	if !mi.IsMSReplica() {
		leader, err := mi.msClient.LeaderAddress()
		if err != nil {
			return err
		}

		if !strings.HasPrefix(leader, "localhost") {
			msg += ", try " + leader
		}

		return errors.New(msg)
	}

	return nil
}

// BioHealthQuery implements the method defined for the Management Service.
func (svc *mgmtSvc) BioHealthQuery(ctx context.Context, req *mgmtpb.BioHealthReq) (*mgmtpb.BioHealthResp, error) {
	svc.log.Debugf("MgmtSvc.BioHealthQuery dispatch, req:%+v\n", *req)

	// Iterate through the list of local I/O server instances, looking for
	// the first one that successfully fulfills the request. If none succeed,
	// return an error.
	for _, i := range svc.harness.Instances() {
		dresp, err := i.CallDrpc(drpc.ModuleMgmt, drpc.MethodBioHealth, req)
		if err != nil {
			return nil, err
		}

		resp := &mgmtpb.BioHealthResp{}
		if err = proto.Unmarshal(dresp.Body, resp); err != nil {
			return nil, errors.Wrap(err, "unmarshal BioHealthQuery response")
		}

		if resp.Status == 0 {
			return resp, nil
		}
	}

	reqID := func() string {
		if req.DevUuid != "" {
			return req.DevUuid
		}
		return req.TgtId
	}
	return nil, errors.Errorf("no rank matched request for %q", reqID())
}

// SmdListDevs implements the method defined for the Management Service.
func (svc *mgmtSvc) SmdListDevs(ctx context.Context, req *mgmtpb.SmdDevReq) (*mgmtpb.SmdDevResp, error) {
	svc.log.Debugf("MgmtSvc.SmdListDevs dispatch, req:%+v\n", *req)

	fullResp := new(mgmtpb.SmdDevResp)

	// Iterate through the list of local I/O server instances, and aggregate
	// results into a single response.
	for _, i := range svc.harness.Instances() {
		dresp, err := i.CallDrpc(drpc.ModuleMgmt, drpc.MethodSmdDevs, req)
		if err != nil {
			return nil, err
		}

		instResp := new(mgmtpb.SmdDevResp)
		if err = proto.Unmarshal(dresp.Body, instResp); err != nil {
			return nil, errors.Wrap(err, "unmarshal SmdListDevs response")
		}

		if instResp.Status != 0 {
			return instResp, nil
		}

		fullResp.Devices = append(fullResp.Devices, instResp.Devices...)
	}

	return fullResp, nil
}

// SmdListPools implements the method defined for the Management Service.
func (svc *mgmtSvc) SmdListPools(ctx context.Context, req *mgmtpb.SmdPoolReq) (*mgmtpb.SmdPoolResp, error) {
	svc.log.Debugf("MgmtSvc.SmdListPools dispatch, req:%+v\n", *req)

	fullResp := new(mgmtpb.SmdPoolResp)

	// Iterate through the list of local I/O server instances, and aggregate
	// results into a single response.
	for _, i := range svc.harness.Instances() {
		dresp, err := i.CallDrpc(drpc.ModuleMgmt, drpc.MethodSmdPools, req)
		if err != nil {
			return nil, err
		}

		instResp := new(mgmtpb.SmdPoolResp)
		if err = proto.Unmarshal(dresp.Body, instResp); err != nil {
			return nil, errors.Wrap(err, "unmarshal SmdListPools response")
		}

		if instResp.Status != 0 {
			return instResp, nil
		}

		fullResp.Pools = append(fullResp.Pools, instResp.Pools...)
	}

	return fullResp, nil
}

// DevStateQuery implements the method defined for the Management Service.
func (svc *mgmtSvc) DevStateQuery(ctx context.Context, req *mgmtpb.DevStateReq) (*mgmtpb.DevStateResp, error) {
	svc.log.Debugf("MgmtSvc.DevStateQuery dispatch, req:%+v\n", *req)

	// Iterate through the list of local I/O server instances, looking for
	// the first one that successfully fulfills the request. If none succeed,
	// return an error.
	for _, i := range svc.harness.Instances() {
		dresp, err := i.CallDrpc(drpc.ModuleMgmt, drpc.MethodDevStateQuery, req)
		if err != nil {
			return nil, err
		}

		resp := &mgmtpb.DevStateResp{}
		if err = proto.Unmarshal(dresp.Body, resp); err != nil {
			return nil, errors.Wrap(err, "unmarshal DevStateQuery response")
		}

		if resp.Status == 0 {
			return resp, nil
		}
	}

	return nil, errors.Errorf("no rank matched request for %q", req.DevUuid)
}

// StorageSetFaulty implements the method defined for the Management Service.
func (svc *mgmtSvc) StorageSetFaulty(ctx context.Context, req *mgmtpb.DevStateReq) (*mgmtpb.DevStateResp, error) {
	svc.log.Debugf("MgmtSvc.StorageSetFaulty dispatch, req:%+v\n", *req)

	// Iterate through the list of local I/O server instances, looking for
	// the first one that successfully fulfills the request. If none succeed,
	// return an error.
	for _, i := range svc.harness.Instances() {
		dresp, err := i.CallDrpc(drpc.ModuleMgmt, drpc.MethodSetFaultyState, req)
		if err != nil {
			return nil, err
		}

		resp := &mgmtpb.DevStateResp{}
		if err = proto.Unmarshal(dresp.Body, resp); err != nil {
			return nil, errors.Wrap(err, "unmarshal StorageSetFaulty response")
		}

		if resp.Status == 0 {
			return resp, nil
		}
	}

	return nil, errors.Errorf("no rank matched request for %q", req.DevUuid)
}

// ListContainers implements the method defined for the Management Service.
func (svc *mgmtSvc) ListContainers(ctx context.Context, req *mgmtpb.ListContReq) (*mgmtpb.ListContResp, error) {
	svc.log.Debugf("MgmtSvc.ListContainers dispatch, req:%+v\n", *req)

	mi, err := svc.harness.GetMSLeaderInstance()
	if err != nil {
		return nil, err
	}

	dresp, err := mi.CallDrpc(drpc.ModuleMgmt, drpc.MethodListContainers, req)
	if err != nil {
		return nil, err
	}

	resp := &mgmtpb.ListContResp{}
	if err = proto.Unmarshal(dresp.Body, resp); err != nil {
		return nil, errors.Wrap(err, "unmarshal ListContainers response")
	}

	svc.log.Debugf("MgmtSvc.ListContainers dispatch, resp:%+v\n", *resp)

	return resp, nil
}
