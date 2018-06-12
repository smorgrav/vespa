// Copyright 2018 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.hosted.athenz.instanceproviderservice.identitydocument;


import com.google.common.collect.ImmutableSet;
import com.yahoo.component.Version;
import com.yahoo.config.provision.ApplicationId;
import com.yahoo.config.provision.ApplicationName;
import com.yahoo.config.provision.ClusterMembership;
import com.yahoo.config.provision.Environment;
import com.yahoo.config.provision.InstanceName;
import com.yahoo.config.provision.NodeType;
import com.yahoo.config.provision.RegionName;
import com.yahoo.config.provision.SystemName;
import com.yahoo.config.provision.TenantName;
import com.yahoo.config.provision.Zone;
import com.yahoo.vespa.athenz.identityprovider.api.EntityBindingsMapper;
import com.yahoo.vespa.athenz.identityprovider.api.IdentityType;
import com.yahoo.vespa.athenz.identityprovider.api.SignedIdentityDocument;
import com.yahoo.vespa.athenz.identityprovider.api.VespaUniqueInstanceId;
import com.yahoo.vespa.athenz.identityprovider.api.bindings.SignedIdentityDocumentEntity;
import com.yahoo.vespa.hosted.athenz.instanceproviderservice.AutoGeneratedKeyProvider;
import com.yahoo.vespa.hosted.athenz.instanceproviderservice.config.AthenzProviderServiceConfig;
import com.yahoo.vespa.hosted.athenz.instanceproviderservice.instanceconfirmation.InstanceValidator;
import com.yahoo.vespa.hosted.provision.Node;
import com.yahoo.vespa.hosted.provision.NodeRepository;
import com.yahoo.vespa.hosted.provision.node.Allocation;
import com.yahoo.vespa.hosted.provision.node.Generation;
import com.yahoo.vespa.hosted.provision.testutils.MockNodeFlavors;
import org.hamcrest.Matchers;
import org.junit.Test;

import java.util.HashSet;
import java.util.Optional;

import static com.yahoo.vespa.hosted.athenz.instanceproviderservice.TestUtils.getAthenzProviderConfig;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertTrue;
import static org.mockito.Matchers.eq;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

/**
 * @author valerijf
 */
public class IdentityDocumentGeneratorTest {
    private static final Zone ZONE = new Zone(SystemName.cd, Environment.dev, RegionName.from("us-north-1"));

    @Test
    public void generates_valid_identity_document() throws Exception {
        String parentHostname = "docker-host";
        String containerHostname = "docker-container";

        ApplicationId appid = ApplicationId.from(
                TenantName.from("tenant"), ApplicationName.from("application"), InstanceName.from("default"));
        Allocation allocation = new Allocation(appid,
                                               ClusterMembership.from("container/default/0/0", Version.fromString("1.2.3")),
                                               Generation.inital(),
                                               false);
        Node parentNode = Node.create("ostkid",
                                      ImmutableSet.of("127.0.0.1"),
                                      new HashSet<>(),
                                      parentHostname,
                                      Optional.empty(),
                                      new MockNodeFlavors().getFlavorOrThrow("default"),
                                      NodeType.host);
        Node containerNode = Node.createDockerNode("docker-1",
                                                   ImmutableSet.of("::1"),
                                                   new HashSet<>(),
                                                   containerHostname,
                                                   Optional.of(parentHostname),
                                                   new MockNodeFlavors().getFlavorOrThrow("default"),
                                                   NodeType.tenant)
                .with(allocation);
        NodeRepository nodeRepository = mock(NodeRepository.class);
        when(nodeRepository.getNode(eq(parentHostname))).thenReturn(Optional.of(parentNode));
        when(nodeRepository.getNode(eq(containerHostname))).thenReturn(Optional.of(containerNode));
        AutoGeneratedKeyProvider keyProvider = new AutoGeneratedKeyProvider();

        String dnsSuffix = "vespa.dns.suffix";
        AthenzProviderServiceConfig config = getAthenzProviderConfig("domain", "service", dnsSuffix, ZONE);
        IdentityDocumentGenerator identityDocumentGenerator =
                new IdentityDocumentGenerator(config, nodeRepository, ZONE, keyProvider);
        SignedIdentityDocument signedIdentityDocument = identityDocumentGenerator.generateSignedIdentityDocument(containerHostname, IdentityType.TENANT);

        // Verify attributes
        assertEquals(containerHostname, signedIdentityDocument.instanceHostname());

        String environment = "dev";
        String region = "us-north-1";
        String expectedZoneDnsSuffix = environment + "-" + region + "." + dnsSuffix;
        assertEquals(expectedZoneDnsSuffix, signedIdentityDocument.dnsSuffix());

        VespaUniqueInstanceId expectedProviderUniqueId =
                new VespaUniqueInstanceId(0, "default", "default", "application", "tenant", region, environment, IdentityType.TENANT);
        assertEquals(expectedProviderUniqueId, signedIdentityDocument.providerUniqueId());

        // Validate that container ips are present
        assertThat(signedIdentityDocument.ipAddresses(), Matchers.containsInAnyOrder("::1"));

        SignedIdentityDocumentEntity signedIdentityDocumentEntity = EntityBindingsMapper.toSignedIdentityDocumentEntity(signedIdentityDocument);

        // Validate signature
        assertTrue("Message", InstanceValidator.isSignatureValid(keyProvider.getPublicKey(0),
                                                                 signedIdentityDocumentEntity.rawIdentityDocument,
                                                                 signedIdentityDocument.signature()));

    }
}
